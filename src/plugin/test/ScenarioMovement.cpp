// ScenarioMovement.cpp - squad movement + presence scenarios (monolith split
// from Scenario.cpp, 2026-07-12): leader_move, fast_march, coop_presence,
// travel_parity, split_interest, split_far. Classes are TU-private (anonymous
// namespace); only makeMovementScenario (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// leader_move (Stage 1): the HOST orders its squad leader to walk to a nearby
// destination and streams its transform; the JOIN drives its local copy of that
// same (shared-save) leader to the received transform. Host logs MEMBER, join
// logs RECV; the runner cross-checks them within tolerance.
class LeaderMoveScenario : public TimedScenario {
public:
    LeaderMoveScenario()
        : TimedScenario("leader_move", 500),
          started_(false), recvCount_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        started_ = true;
        if (ctx.isHost) {
            Character* ld = engine::leader(ctx.gw);
            if (ld && engine::readPos(ld, &sx_, &sy_, &sz_)) {
                haveStart_ = true;
                engine::orderMoveTo(ld, sx_ + LEG, sy_, sz_ + LEG);
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Emit a MEMBER/RECV line ~2 Hz so the runner has positions to compare
        // and an anchor to time its screenshot.
        if (evidenceDue(ctx.elapsedMs)) {
            Character* ld = engine::leader(ctx.gw);
            if (ctx.isHost) {
                // Oscillate between the start point and a far offset so the leader
                // keeps translating for the whole window (the later-loading join
                // then sees LIVE, sustained walking - the fair test for engine-
                // driven locomotion). Long legs + a long half-period keep straight
                // walking dominant and reversals rare (a reversal is a legitimate
                // stop/turn for engine locomotion, but we want them sparse). Then
                // SETTLE: return to the start and halt so the host streams a STILL
                // pose and the join converges for a fair cross-check.
                if (haveStart_ && ld) {
                    if (ctx.elapsedMs >= DURATION_MS - SETTLE_MS) {
                        engine::orderMoveTo(ld, sx_, sy_, sz_);
                    } else {
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        float tx = legB ? (sx_ + LEG) : sx_;
                        float tz = legB ? (sz_ + LEG) : sz_;
                        engine::orderMoveTo(ld, tx, sy_, tz);
                    }
                }
                logScenarioLine("MEMBER", ld);
            } else {
                if (logScenarioLine("RECV", ld)) ++recvCount_;
            }
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            if (ctx.isHost) {
                // Authoritative side passes if its leader resolved (and, if we
                // had a start, ideally moved). Position match is the runner's job.
                passed_ = (engine::leader(ctx.gw) != 0);
            } else {
                passed_ = (recvCount_ >= 1); // observed + applied at least once
            }
            return true;
        }
        return false;
    }

private:
    // 24s -> 62s (2026-07-10): the join's session-start clock catch-up slews
    // its sim at up to 2x for the first ~35-40 s, and the smoothness oracle
    // now EXCLUDES frames scored during the slew (they measured the transient,
    // not the interp pipeline). The window must extend well past convergence
    // so the gate still scores a real steady-state sample (>= 200 frames).
    static const unsigned long DURATION_MS = 62000;
    static const unsigned long SETTLE_MS   = 8000;  // final halt window (fair cross-check + converge)
    static const unsigned long LEG_MS      = 6000;  // oscillation half-period (sparse reversals)
    static const float         LEG;                 // straight-walk leg length (units)
    bool          started_;
    unsigned int  recvCount_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};

const float LeaderMoveScenario::LEG = 14.0f;

// fast_march (2026-07-11 rubber-banding validation): leader_move at 5x game
// speed. Speed consensus is min(host, join), so BOTH sides vote 5x through the
// loud simulated-click path (writeGameSpeed - the intent hooks capture it as a
// user request, exactly like the manual-session repro). The host marches its
// leader in oscillating legs; the join drives its copy from the stream. The
// verdict rides the join's [interp] counters via the snap_rate oracle: before
// the velocity-aware snap gate, 5x wall-clock velocities turned the fixed 8 u
// hard-snap gate into a per-sample teleport (~35 snaps/s measured 2026-07-11).
class FastMarchScenario : public Scenario {
public:
    FastMarchScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastVoteMs_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual const char* name() const { return "fast_march"; }

    virtual void onStart(const ScenarioContext& ctx) {
        engine::writeGameSpeed(ctx.gw, 5.0f, false); // our 5x vote (both sides)
        if (ctx.isHost) {
            Character* ld = engine::leader(ctx.gw);
            if (ld && engine::readPos(ld, &sx_, &sy_, &sz_)) {
                haveStart_ = true;
                engine::orderMoveTo(ld, sx_ + LEG, sy_, sz_ + LEG);
            }
        }
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FASTMARCH vote=5.0 haveStart=%d",
                  haveStart_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool settling = ctx.elapsedMs >= DURATION_MS - SETTLE_MS;
        // Re-vote 5x every 5 s (an engine event may have reset our request);
        // in the settle window vote back to 1x so the session ends sane.
        if (ctx.elapsedMs - lastVoteMs_ >= 5000 || lastVoteMs_ == 0) {
            lastVoteMs_ = ctx.elapsedMs;
            engine::writeGameSpeed(ctx.gw, settling ? 1.0f : 5.0f, false);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* ld = engine::leader(ctx.gw);
            if (ctx.isHost) {
                if (haveStart_ && ld) {
                    if (settling) {
                        engine::orderMoveTo(ld, sx_, sy_, sz_);
                    } else {
                        // Time-based oscillation: at 5x the leader covers a leg
                        // in ~1 s and rests until the flip - bursts of genuine
                        // 5x sprinting are exactly the old snap-storm repro.
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        engine::orderMoveTo(ld, legB ? sx_ + LEG : sx_, sy_,
                                                legB ? sz_ + LEG : sz_);
                    }
                }
                logScenarioLine("MEMBER", ld);
            } else {
                if (logScenarioLine("RECV", ld)) ++recvCount_;
            }
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            engine::writeGameSpeed(ctx.gw, 1.0f, false); // leave the world at 1x
            passed_ = ctx.isHost ? (engine::leader(ctx.gw) != 0)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long DURATION_MS = 62000; // outlast the clock slew
    static const unsigned long SETTLE_MS   = 8000;  // final 1x halt window
    static const unsigned long LEG_MS      = 4000;  // oscillation half-period
    static const float         LEG;                 // leg length (units)
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastVoteMs_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};

const float FastMarchScenario::LEG = 25.0f;

// coop_presence (Phase 3.5, BIDIRECTIONAL presence - the keystone two-player test):
// both clients MOVE their OWNED squad member (chosen by save-stable hand-rank, the
// same ordering the Replicator partitions on: host owns rank 0, join owns rank 1 -
// leader-first) and stream it, while driving + observing the PEER's owned member.
// Each side logs MEMBER for its OWN member (authoritative truth it streams) and RECV
// for the PEER's member (the local driven copy), so the runner cross-checks BOTH
// directions by hand: host MEMBER(rank0) vs join RECV(rank0), and join MEMBER(rank1)
// vs host RECV(rank1). Proves each player's character is present + correctly placed
// on the other client. Requires a shared save with >=2 controllable squad members.
class CoopPresenceScenario : public TimedScenario {
public:
    CoopPresenceScenario()
        : TimedScenario("coop_presence", 500), recvCount_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank  = ctx.isHost ? 0u : 1u; // our squad-tab rank
        const unsigned int peerRank = ctx.isHost ? 1u : 0u; // the peer's squad-tab rank

        if (evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, /*leaderOnly*/ false, sq, MAX_SQUAD);

            // Classify every shared-squad member by its SQUAD-TAB rank (same key the
            // Replicator partitions on: distinct hand-containers, sorted). Log MEMBER
            // for ALL members in OUR tab(s) (authoritative truth we stream) and RECV
            // for ALL members in the PEER's tab(s) (the bodies we drive). Pick the
            // lowest-hand owned member as the MOVER so the peer sees sustained motion.
            int leaderIdx = -1; bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int cr = containerRankOf(sq, n, i);
                if (cr < 0) continue;
                if ((unsigned int)cr == ownRank) {
                    logScenarioEntity("MEMBER", sq[i]);
                    if (leaderIdx < 0 || handLess(sq[i], sq[leaderIdx])) leaderIdx = (int)i;
                } else if ((unsigned int)cr == peerRank) {
                    logScenarioEntity("RECV", sq[i]); sawPeer = true;
                }
            }
            if (sawPeer) ++recvCount_;

            // Move our tab leader so the peer sees LIVE motion, then settle to the start
            // so both clients converge for a clean stationary cross-check in the
            // overlapping window (movement proves presence; the settle proves placement).
            if (leaderIdx >= 0) {
                Character* oc = engine::resolve(sq[leaderIdx]);
                if (oc) {
                    if (!haveStart_) {
                        sx_ = sq[leaderIdx].x; sy_ = sq[leaderIdx].y; sz_ = sq[leaderIdx].z;
                        haveStart_ = true;
                    }
                    if (ctx.elapsedMs < MOVE_MS) {
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        engine::orderMoveTo(oc, legB ? sx_ + LEG : sx_, sy_,
                                                legB ? sz_ + LEG : sz_);
                    } else {
                        engine::orderMoveTo(oc, sx_, sy_, sz_); // settle back to start
                    }
                }
            }
        }

        // The host outlives the join (keeps streaming through the join's whole window
        // so the join's RECV never goes stale); the join reports the verdict.
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = haveStart_ && (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
    // Full hand order (used only to pick a stable per-tab "leader" mover).
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    // Squad-tab identity = the hand CONTAINER (hContainer,hContainerSerial).
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    // Rank of member i's SQUAD TAB among the distinct, sorted containers (O(n^2),
    // squads are tiny). MUST match the Replicator's container-rank partition so the
    // scenario's "owned" set is exactly the set the Replicator streams as owned.
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
    static const unsigned long JOIN_DURATION_MS = 24000;
    static const unsigned long MOVE_MS          = 12000; // oscillate, then settle
    static const unsigned long LEG_MS           = 4000;  // oscillation half-period
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         LEG;
    unsigned int  recvCount_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};
const float CoopPresenceScenario::LEG = 12.0f;

// travel_parity (2026-07-11 field report, "yellow packs while roaming"): the
// JOIN's player character travels FAR from the start while the HOST's PC
// follows - the roaming direction no automated test exercised (every mover so
// far was host-side, but in free play it is the JOIN that wanders and drags
// the interest/census coverage with it). The join TELEPORT-HOPS its OWN
// rank-1 tab leader across the map (the split_interest engine::park
// precedent): HOPS legs of HOP u with a HOP_DWELL_MS dwell each, ~60,000 u
// total - every hop lands entirely OUTSIDE the previous 2000 u census
// bubble, so existence coverage must rebuild from nothing at each stop
// (zone streaming, census re-centering, mint/cull churn - a compressed
// cross-map trek). The host follows its LOCAL driven copy of the join
// leader: teleport catch-up (park) when the gap exceeds FOLLOW_SNAP,
// orderMoveTo otherwise, logging "SCENARIO FOLLOW self=.. peer=.. gap=.."
// for the follow-quality gate.
// While the pair travels, BOTH sides dump a 5 s worldstate (SCENARIO WORLD /
// WNPC rows - the host from its census walk with cls=host, the join from the
// existence audit with each NPC's authority class; enabled via
// Replicator::setAuditRows when this scenario is armed) so Test-TravelParity
// can measure join-only ghosts under zone streaming + census re-centering,
// exactly the free-play failure mode.
class TravelParityScenario : public TimedScenario {
public:
    TravelParityScenario()
        : TimedScenario("travel_parity", 1000), recvCount_(0), hopsDone_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        // Anchor = the MOVER's start: the join's rank-1 tab leader (both
        // clients resolve it locally from the shared save).
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int mv = tabLeaderIdx(sq, n, 1);
        if (mv >= 0) {
            haveAnchor_ = true;
            ax_ = sq[mv].x; ay_ = sq[mv].y; az_ = sq[mv].z;
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO TRAVEL anchor=%.1f,%.1f,%.1f have=%d hop=%.0f hops=%u dwell=%lums",
                  ax_, ay_, az_, haveAnchor_ ? 1 : 0, HOP,
                  (unsigned)HOPS, HOP_DWELL_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO TRAVEL needs a 2-tab save (rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;

        if (haveAnchor_ && evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int mv = tabLeaderIdx(sq, n, 1); // the join's mover
            int fl = tabLeaderIdx(sq, n, 0); // the host's follower

            if (!ctx.isHost) {
                // JOIN: hop our own tab leader one HOP further out every
                // HOP_DWELL_MS (park = halt + teleport; the dwell gives zone
                // streaming + census/mint a re-coverage window at each stop,
                // and a short walk order after the park re-grounds the body
                // and keeps it a live, moving subject rather than a statue).
                unsigned int wantHops = (unsigned int)(ctx.elapsedMs / HOP_DWELL_MS);
                if (wantHops > HOPS) wantHops = HOPS;
                if (mv >= 0) {
                    Character* c = engine::resolve(sq[mv]);
                    if (c) {
                        if (wantHops > hopsDone_) {
                            hopsDone_ = wantHops;
                            float hx = ax_ + (float)hopsDone_ * HOP;
                            engine::park(c, hx, sq[mv].y, az_, 0.0f);
                            char b[96];
                            _snprintf(b, sizeof(b) - 1,
                                      "SCENARIO HOP n=%u to=%.0f,%.0f,%.0f",
                                      hopsDone_, hx, sq[mv].y, az_);
                            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                        } else {
                            // Walk a short leg inside the dwell (re-grounds
                            // the parked body; a genuinely moving mover).
                            float hx = ax_ + (float)hopsDone_ * HOP;
                            bool legB = ((ctx.elapsedMs / 3000) % 2) != 0;
                            engine::orderMoveTo(c, hx + (legB ? 15.0f : 0.0f),
                                                sq[mv].y, az_);
                        }
                    }
                    logScenarioEntity("MEMBER", sq[mv]);
                }
                if (fl >= 0) { logScenarioEntity("RECV", sq[fl]); ++recvCount_; }
            } else {
                // HOST: chase the join leader's LOCAL driven copy - the same
                // body free-play players follow on screen. A hop opens a
                // multi-thousand-unit gap no walk can close: teleport
                // catch-up past FOLLOW_SNAP, walk inside it, stand inside
                // FOLLOW_STOP (don't shove the driven copy around).
                if (mv >= 0 && fl >= 0) {
                    float dx = sq[mv].x - sq[fl].x, dz = sq[mv].z - sq[fl].z;
                    float gap = (float)sqrt((double)(dx * dx + dz * dz));
                    Character* c = engine::resolve(sq[fl]);
                    if (c && gap > FOLLOW_SNAP) {
                        engine::park(c, sq[mv].x - FOLLOW_STOP, sq[mv].y,
                                     sq[mv].z, 0.0f);
                    } else if (c && gap > FOLLOW_STOP) {
                        float f = (gap - FOLLOW_STOP) / gap;
                        engine::orderMoveTo(c, sq[fl].x + dx * f, sq[mv].y,
                                                sq[fl].z + dz * f);
                    }
                    char b[160];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO FOLLOW self=%.1f,%.1f,%.1f peer=%.1f,%.1f,%.1f gap=%.1f",
                              sq[fl].x, sq[fl].y, sq[fl].z,
                              sq[mv].x, sq[mv].y, sq[mv].z, gap);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    logScenarioEntity("MEMBER", sq[fl]);
                    logScenarioEntity("RECV", sq[mv]); ++recvCount_;
                }
            }
        }

        if (ctx.elapsedMs >= dur) {
            passed_ = haveAnchor_ && recvCount_ >= 1;
            return true;
        }
        return false;
    }

private:
    // Long windows: the manifest entry raises the runner's self-exit backstop
    // (Seconds=220) and kill grace (KillGraceSec=190) for this scenario, so
    // the 160 s host window survives. 15 hops x 4000 u = 60,000 u in ~135 s
    // of hop cadence, then a dwell at the far point.
    static const unsigned long JOIN_DURATION_MS = 150000; // hops + far dwell
    static const unsigned long HOST_DURATION_MS = 160000; // outlive the join
    static const unsigned long HOP_DWELL_MS     = 9000;   // per-stop coverage window
    static const unsigned int  HOPS             = 15;     // total legs
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         HOP;         // leg length (units)
    static const float         FOLLOW_STOP; // stop short of the peer (units)
    static const float         FOLLOW_SNAP; // teleport catch-up past this gap
    unsigned int  recvCount_;
    unsigned int  hopsDone_;
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};
const float TravelParityScenario::HOP         = 4000.0f;
const float TravelParityScenario::FOLLOW_STOP = 12.0f;
const float TravelParityScenario::FOLLOW_SNAP = 150.0f;

// split_interest (step 5, dual-interest conformance): the players SPLIT UP and the
// shared world must keep streaming around BOTH of them. The HOST relocates its
// whole owned tab (rank 0) ~260 u away from the bar and holds it there; the JOIN's
// tab (rank 1) stays with the bar NPCs. Under the old single-host-leader interest
// sphere the bar leaves the host's capture radius and its NPCs stop streaming -
// exactly spike 16's degradation. With dual-interest (one sphere per tab leader)
// the host's SECOND sphere - centered on the join's tab-1 member, resolved locally
// from the shared save - keeps the bar streamed. Both sides log the standard NPC
// MEMBER/RECV series; the runner's SPLIT-INTEREST oracle checks that bar-anchored
// NPCs (near the logged bar anchor) still track AFTER the host moved away.
class SplitInterestScenario : public TimedScenario {
public:
    SplitInterestScenario()
        : TimedScenario("split_interest", 500), recvCount_(0),
          movedLogged_(false), haveBar_(false), bx_(0), by_(0), bz_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        // Bar anchor = the rank-1 tab leader's position (the member that STAYS).
        // Logged by both clients so the oracle can select bar-anchored NPCs.
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            if (containerRankOf(sq, n, i) == 1) {
                haveBar_ = true; bx_ = sq[i].x; by_ = sq[i].y; bz_ = sq[i].z;
                break;
            }
        }
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPLIT bar=%.2f,%.2f,%.2f have=%d",
                  bx_, by_, bz_, haveBar_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveBar_)
            coop::logLine("SCENARIO SPLIT needs a 2-tab save (rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST: from MOVE_AT_MS, hold every rank-0 member at the remote point.
        // Teleport-park (not walk) so the split is deterministic and immediate.
        if (ctx.isHost && haveBar_ && ctx.elapsedMs >= MOVE_AT_MS) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            unsigned int moved = 0;
            for (unsigned int i = 0; i < n; ++i) {
                if (containerRankOf(sq, n, i) != 0) continue;
                Character* c = engine::resolve(sq[i]);
                if (!c) continue;
                float rx = bx_ + SPLIT_DIST + (float)moved * 3.0f;
                float d2 = (sq[i].x - rx) * (sq[i].x - rx) + (sq[i].z - bz_) * (sq[i].z - bz_);
                if (d2 > 4.0f) engine::park(c, rx, by_, bz_, 0.0f);
                ++moved;
            }
            if (!movedLogged_ && moved > 0) {
                movedLogged_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPLIT moved=%u to=%.2f,%.2f,%.2f",
                          moved, bx_ + SPLIT_DIST, by_, bz_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // Standard NPC series on both sides (captureNpcs is dual-sphere now, so
        // the host's MEMBER set must keep covering the bar after the move).
        if (evidenceDue(ctx.elapsedMs)) {
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) passed_ = haveBar_ && movedLogged_;
            else            passed_ = haveBar_ && (recvCount_ >= 1);
            return true;
        }
        return false;
    }

private:
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

    static const unsigned long MOVE_AT_MS       = 8000;  // baseline, then split
    static const unsigned long HOST_DURATION_MS = 60000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const unsigned int  MAX_LOG          = 40;
    static const float         SPLIT_DIST;               // how far rank-0 relocates

    unsigned int  recvCount_;
    bool          movedLogged_;
    bool          haveBar_;
    float         bx_, by_, bz_;
};

const float SplitInterestScenario::SPLIT_DIST = 260.0f;

// split_far (2026-08-02 field report, "long sessions with far travel go out of
// sync - the join sees local NPCs the host does not"): the two squads separate
// BEYOND the census radius and BOTH HOLD there, each in a populated region.
//
// No existing scenario creates that condition, which is why none of them ever
// reproduced the symptom:
//   - split_interest separates by SPLIT_DIST (260 u). That clears the ~200 u
//     stream bubble but sits far INSIDE the 2000 u census sphere, so there is
//     still only one interest cluster and the census covers both tabs.
//   - travel_parity moves the join ~60,000 u, but the HOST FOLLOWS it (measured
//     median gap ~14 u), so again one cluster - and its straight-ray hop
//     corridor is mostly wilderness, so there are no NPCs to disagree about.
//
// The mechanism under test is that Kenshi streams zones around the LOCAL
// camera. The host's census walks every interest anchor including the peer's,
// but a sphere query at the peer's anchor runs against whatever the HOST's
// engine has loaded. Once the pair separates by more than the loaded-zone
// footprint, the host finds nothing there and publishes "no NPCs here" for a
// place the join may be standing in the middle of a town - and the join's real
// local bodies fall into the ghost bucket.
//
// Script (join): hop the rank-1 tab leader outward on a golden-angle spiral,
//   MIN_SEP u out and widening. Mid-dwell (after the zone has had time to
//   stream) probe the local population with engine::countNpcsNear - a single
//   sphere around the stop, deliberately independent of the interest anchors
//   whose budgeting is itself under test. SETTLE at the first genuinely
//   inhabited stop and hold there; an empty stop teaches nothing.
// Script (host): HOLD the rank-0 tab at its start (the 'sync' bar - the
//   populated host anchor). Explicitly no follow: the pair must not reunite.
// Both sides log SCENARIO SPLITFAR rows carrying the separation, the population
// each side can see at BOTH tab leaders, and whether the mover's zone is loaded
// locally. The host's popMover/moverZone pair is the direct measurement: host
// popMover=0 moverZone=0 while the join reports a crowd is the failure, in one
// line. Both sides also dump the 5 s worldstate rows (setAuditRows).
class SplitFarScenario : public TimedScenario {
public:
    SplitFarScenario()
        : TimedScenario("split_far", 1000), recvCount_(0), hopsDone_(0),
          settled_(false), settlePop_(0), bestPop_(0), emptyProbes_(0),
          exhausted_(false), camMs_(0), camPhase_(0), hopMs_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0),
          hx_(0), hy_(0), hz_(0), sx_(0), sz_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int mv = tabLeaderIdx(sq, n, 1); // the tab that LEAVES (join-owned)
        int st = tabLeaderIdx(sq, n, 0); // the tab that STAYS (host-owned)
        if (mv >= 0 && st >= 0) {
            haveAnchor_ = true;
            ax_ = sq[mv].x; ay_ = sq[mv].y; az_ = sq[mv].z;
            hx_ = sq[st].x; hy_ = sq[st].y; hz_ = sq[st].z;
            sx_ = ax_; sz_ = az_;
        }
        unsigned int popStay0 = 0, popMove0 = 0;
        float sep0 = 0.0f;
        if (haveAnchor_) {
            popStay0 = engine::countNpcsNear(ctx.gw, hx_, hy_, hz_, PROBE_R);
            popMove0 = engine::countNpcsNear(ctx.gw, ax_, ay_, az_, PROBE_R);
            float dx = ax_ - hx_, dz = az_ - hz_;
            sep0 = (float)sqrt((double)(dx * dx + dz * dz));
            // A save that ALREADY starts the pair apart is authoritative about
            // where they belong: hold, and spend the whole window measuring.
            // Deliberately keyed on separation ALONE. Gating it on the starting
            // population too made the scenario spiral away from a good fixture,
            // because at t=0 the remote zone has not finished streaming and the
            // probe reads a fraction of the eventual crowd. Whether the region
            // turned out populated enough to judge is the oracle's call, made
            // over the whole window rather than one cold sample.
            if (sep0 >= MIN_SEP) {
                settled_ = true; exhausted_ = true;
                sx_ = ax_; sz_ = az_;
            }
        }
        char b[240];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SPLITFAR start have=%d stay=%.0f,%.0f,%.0f "
                  "popStay=%u popMover=%u sep=%.0f preSplit=%d "
                  "probeR=%.0f popMin=%u hops=%u minSep=%.0f",
                  haveAnchor_ ? 1 : 0, hx_, hy_, hz_, popStay0, popMove0, sep0,
                  settled_ ? 1 : 0, PROBE_R, (unsigned)POP_MIN, (unsigned)HOPS,
                  MIN_SEP);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO SPLITFAR needs a 2-tab save (rank-0/rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;

        if (haveAnchor_ && evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int mv = tabLeaderIdx(sq, n, 1);
            int st = tabLeaderIdx(sq, n, 0);

            // Camera placement, in three phases. park() teleports bodies but
            // never the camera, so before this the pair simply watched whatever
            // the save started on - run 20260802_112449 had BOTH cameras on one
            // point for the whole window, thousands of units from the join's
            // characters. That is not a configuration anyone plays in, and it
            // suppresses the very thing under test: the camera contributes an
            // interest anchor, and rendering is camera-driven, so a parked run
            // measures enumeration in a region neither client is drawing.
            //
            //   own        host->stay,  join->mover  (normal play)
            //   both_stay  host->stay,  join->stay   (OVERLAP on the host's)
            //   both_mover host->mover, join->mover  (OVERLAP on the join's)
            //   back       host->stay,  join->mover  (normal play again)
            //
            // The overlap phases are the point. Swapping BOTH cameras at once
            // only mirrors the disjointness - the two clients still never draw
            // the same character, so a disagreement there is unattributable:
            // it could be a replication miss or simply the other side not
            // looking. Pointing both at ONE squad removes the confound, and it
            // is the only configuration in which "these two clients disagree
            // about what is here" means only one thing.
            //
            // both_mover doubles as the causal probe: if the HOST's count at
            // the mover rises when the host looks there, camera presence is
            // what materialises the bodies. 'back' then asks whether they
            // SURVIVE the camera leaving - if they do, each client accumulates
            // its own population over a session, which is the reported
            // "drifts out of sync over a long session".
            unsigned int ph = phaseOf(ctx.elapsedMs);
            int tgt;
            if      (ph == 1) tgt = st;                  // both on the stayer
            else if (ph == 2) tgt = mv;                  // both on the mover
            else              tgt = ctx.isHost ? st : mv; // own / back
            if (tgt >= 0 && (camMs_ == 0 || ph != camPhase_ ||
                             (ctx.elapsedMs - camMs_) >= CAM_REFOCUS_MS)) {
                if (ph != camPhase_) {
                    char pb[128];
                    _snprintf(pb, sizeof(pb) - 1,
                              "SCENARIO SPLITFAR phase side=%s phase=%s watching=%s",
                              ctx.isHost ? "host" : "join", phaseName(ph),
                              (tgt == st) ? "stay" : "mover");
                    pb[sizeof(pb) - 1] = '\0'; coop::logLine(pb);
                }
                camMs_ = ctx.elapsedMs; camPhase_ = ph;
                Character* tc = engine::resolve(sq[tgt]);
                if (tc) engine::cameraFocusOn(ctx.gw, tc);
            }

            if (!ctx.isHost) {
                if (mv >= 0) { driveMover(ctx, sq[mv]); logScenarioEntity("MEMBER", sq[mv]); }
                if (st >= 0) { logScenarioEntity("RECV", sq[st]); ++recvCount_; }
            } else {
                // HOST: hold every rank-0 member at the start. Park only on
                // real drift so a settled body is not re-teleported every tick.
                for (unsigned int i = 0; i < n; ++i) {
                    if (tabRankOf(sq, n, i) != 0) continue;
                    Character* c = engine::resolve(sq[i]);
                    if (!c) continue;
                    float dx = sq[i].x - hx_, dz = sq[i].z - hz_;
                    if (dx * dx + dz * dz > HOLD_R * HOLD_R)
                        engine::park(c, hx_, hy_, hz_, 0.0f);
                }
                if (st >= 0) logScenarioEntity("MEMBER", sq[st]);
                if (mv >= 0) { logScenarioEntity("RECV", sq[mv]); ++recvCount_; }
            }

            if (mv >= 0 && st >= 0) {
                float dx = sq[mv].x - sq[st].x, dz = sq[mv].z - sq[st].z;
                float sep = (float)sqrt((double)(dx * dx + dz * dz));
                unsigned int popMover = engine::countNpcsNear(
                    ctx.gw, sq[mv].x, sq[mv].y, sq[mv].z, PROBE_R);
                unsigned int popStay = engine::countNpcsNear(
                    ctx.gw, sq[st].x, sq[st].y, sq[st].z, PROBE_R);
                int zMover = engine::isZoneLoadedAt(ctx.gw, sq[mv].x, sq[mv].y,
                                                    sq[mv].z) ? 1 : 0;
                int zStay = engine::isZoneLoadedAt(ctx.gw, sq[st].x, sq[st].y,
                                                   sq[st].z) ? 1 : 0;
                char b[240];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPLITFAR side=%s hop=%u sep=%.0f "
                          "popMover=%u popStay=%u zMover=%d zStay=%d settled=%d "
                          "phase=%s",
                          ctx.isHost ? "host" : "join", hopsDone_, sep,
                          popMover, popStay, zMover, zStay, settled_ ? 1 : 0,
                          phaseName(phaseOf(ctx.elapsedMs)));
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        if (ctx.elapsedMs >= dur) {
            passed_ = haveAnchor_ && recvCount_ >= 1;
            return true;
        }
        return false;
    }

private:
    // JOIN mover: hop outward until a stop is inhabited, then hold there.
    void driveMover(const ScenarioContext& ctx, const EntityState& mvSt) {
        Character* c = engine::resolve(mvSt);
        if (!c) return;

        if (settled_) {
            // Short alternating leg so the held body stays a live, moving
            // subject instead of a statue (the travel_parity precedent).
            bool legB = ((ctx.elapsedMs / 3000) % 2) != 0;
            engine::orderMoveTo(c, sx_ + (legB ? 15.0f : 0.0f), mvSt.y, sz_);
            // A settle point can EMPTY under us - the first run parked next to
            // a wandering squad that was gone ~20 s later, and the scenario
            // then held a deserted spot for two minutes and had nothing to
            // judge. Re-probe while settled and resume the spiral if the place
            // stays empty, so the window is spent somewhere inhabited.
            if (exhausted_) return; // nowhere left to go - hold and report
            unsigned int pop = engine::countNpcsNear(ctx.gw, mvSt.x, mvSt.y,
                                                     mvSt.z, PROBE_R);
            if (pop >= POP_MIN) { emptyProbes_ = 0; return; }
            if (++emptyProbes_ < EMPTY_TOLERANCE) return;
            settled_ = false; emptyProbes_ = 0;
            hopMs_ = ctx.elapsedMs - HOP_DWELL_MS; // hop again next tick
            char b[144];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SPLITFAR unsettle hop=%u pop=%u (settle point emptied)",
                      hopsDone_, pop);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }

        unsigned long inHop = ctx.elapsedMs - hopMs_;
        if (hopsDone_ == 0 || inHop >= HOP_DWELL_MS) {
            if (hopsDone_ >= HOPS) {
                // Out of hops without finding anyone: settle anyway so the run
                // still holds the pair apart, and report pop=0 - the oracle
                // SKIPs rather than passing on a vacuous empty-corridor run.
                // Terminal, so the empty re-probe above cannot thrash us
                // between settle and unsettle for the rest of the window.
                exhausted_ = true;
                settleHere(mvSt, 0);
                return;
            }
            ++hopsDone_;
            hopMs_ = ctx.elapsedMs;
            hopTarget(hopsDone_, &sx_, &sz_);
            engine::park(c, sx_, mvSt.y, sz_, 0.0f);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SPLITFAR hop n=%u to=%.0f,%.0f", hopsDone_, sx_, sz_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }

        // Probe only once the destination zone has had time to stream in; a
        // probe on the arrival frame reads an unloaded block and always says 0.
        if (inHop >= PROBE_AT_MS) {
            unsigned int pop = engine::countNpcsNear(ctx.gw, mvSt.x, mvSt.y,
                                                     mvSt.z, PROBE_R);
            if (pop > bestPop_) bestPop_ = pop;
            if (pop >= POP_MIN) settleHere(mvSt, pop);
        }
    }

    void settleHere(const EntityState& mvSt, unsigned int pop) {
        settled_ = true; settlePop_ = pop;
        sx_ = mvSt.x; sz_ = mvSt.z;
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SPLITFAR settle hop=%u at=%.0f,%.0f,%.0f pop=%u best=%u",
                  hopsDone_, mvSt.x, mvSt.y, mvSt.z, pop, bestPop_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Viewpoint phase from the elapsed clock. Both sides derive it from the
    // same elapsed ms so the swap happens together; the host's extra window
    // (HOST_DURATION_MS) simply extends 'back'.
    static unsigned int phaseOf(unsigned long ms) {
        unsigned int p = (unsigned int)(ms / PHASE_MS);
        return (p > 3) ? 3 : p;
    }
    static const char* phaseName(unsigned int p) {
        return (p == 0) ? "own"        : (p == 1) ? "both_stay"
             : (p == 2) ? "both_mover" : "back";
    }

    // Golden-angle spiral out from the mover's start. A straight ray (the
    // travel_parity pattern) samples a single corridor and can spend every hop
    // in wilderness; turning ~137.5 degrees per stop spreads them around the
    // surrounding map, so an inhabited place is far likelier to be hit.
    void hopTarget(unsigned int k, float* ox, float* oz) const {
        float ang = (float)k * 2.39996f;
        float r   = MIN_SEP + (float)(k - 1) * STEP_R;
        *ox = ax_ + r * (float)cos((double)ang);
        *oz = az_ + r * (float)sin((double)ang);
    }

    // The manifest raises the runner's self-exit backstop and kill grace for
    // this scenario, as travel_parity does, so the long host window survives.
    static const unsigned long JOIN_DURATION_MS = 180000;
    static const unsigned long HOST_DURATION_MS = 190000; // outlive the join
    static const unsigned long HOP_DWELL_MS     = 8000;   // per-stop window
    static const unsigned long PROBE_AT_MS      = 5500;   // probe late in the dwell
    static const unsigned int  HOPS             = 14;
    static const unsigned int  POP_MIN          = 8;      // "inhabited"
    static const unsigned int  EMPTY_TOLERANCE  = 3;      // ticks before re-hopping
    static const unsigned long CAM_REFOCUS_MS   = 5000;   // re-assert camera follow
    // Viewpoint phase length: four even quarters of the join window. The
    // oracle drops each phase's opening seconds, so what has to fit here is
    // the settling transient (camera move, zone stream, a census round) plus
    // enough steady-state samples to take a median from - not the whole phase.
    static const unsigned long PHASE_MS         = 45000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         MIN_SEP;  // first hop: 2x the census radius
    static const float         STEP_R;   // spiral widening per hop
    static const float         PROBE_R;  // population probe sphere
    static const float         HOLD_R;   // host drift before re-parking

    unsigned int  recvCount_;
    unsigned int  hopsDone_;
    bool          settled_;
    unsigned int  settlePop_;
    unsigned int  bestPop_;
    unsigned int  emptyProbes_;
    bool          exhausted_;   // hop budget spent - settle is now terminal
    unsigned long camMs_;       // last camera re-focus
    unsigned int  camPhase_;    // viewpoint phase the camera was aimed for
    unsigned long hopMs_;
    bool          haveAnchor_;
    float         ax_, ay_, az_; // mover start (spiral origin)
    float         hx_, hy_, hz_; // stayer hold point
    float         sx_, sz_;      // current mover destination
};
const float SplitFarScenario::MIN_SEP = 4000.0f;
const float SplitFarScenario::STEP_R  = 1600.0f;
// 1800 u, not render range: what governs whether the two clients can disagree
// about a body is the 2000 u census/enumeration reach, so the probe measures
// the population inside that shell. The first run probed 900 u and settled next
// to a squad that walked out of it.
const float SplitFarScenario::PROBE_R = 1800.0f;
const float SplitFarScenario::HOLD_R  = 40.0f;

// camp_approach (Phase 2 crash hardening SOAK): reproduce the town/bandit-camp
// approach crash conditions - mint/zone churn on approach plus a real peer drop
// - and prove the survivor cleans up without a fault. Run on the 'camp' save
// (a prison camp, many NPCs). There is NO deterministic repro of the original
// crash, so this is a stress/soak, not a strict-parity gate; Test-CampApproach
// verifies the FIX MECHANISMS from the flushed plugin log.
//
// Timeline (clock from ARM = peer-ready):
//   JOIN teleport-hops its leader across the camp region every HOP_DWELL_MS
//     (park + short walk leg), forcing zone streaming + census/mint bursts as
//     the coverage bubble re-centers - the "approach" churn. It is the machine
//     the crash breadcrumb ("2026-07-11 join crash") blames, so it is the
//     SURVIVOR we harden.
//   HOST holds near its start and self-exits FIRST at HOST_DURATION_MS. That
//     TerminateProcess closes the socket, so the JOIN's transport enqueues a
//     REAL 'handshake: peer left' - firing clearPeerReplicationState (B1) on the
//     survivor mid-churn (no new harness/transport code needed; travel_parity
//     already proves asymmetric self-exit produces a genuine peer-left edge).
//   JOIN keeps running ~JOIN-HOST ms after the drop, so its post-leave cleanup
//     ('[leave] cleared proxies=N') and any stale-drive attempt are captured
//     while it is still hopping (drive path exercised against a just-cleared map).
//
// Test-CampApproach gates (from host.log/join.log): both reach a SCENARIO RESULT
// line (no crash / no truncated log); the join logs 'handshake: peer left' ->
// '[leave] cleared proxies='; no '[drive] STALE' hand is driven after it was
// unbound and no '[drive]' fires after the leave; proxy count returns toward 0.
class CampApproachScenario : public TimedScenario {
public:
    CampApproachScenario()
        : TimedScenario("camp_approach", 1000), hopsDone_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        // Anchor = this side's own squad leader (both clients resolve their own
        // leader locally; camp is not guaranteed to be a 2-tab save, so we do
        // NOT rely on a rank-1 tab the way travel_parity does).
        Character* ld = engine::leader(ctx.gw);
        if (ld && engine::readPos(ld, &ax_, &ay_, &az_)) haveAnchor_ = true;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CAMP start host=%d anchor=%.1f,%.1f,%.1f have=%d "
                  "hop=%.0f hops=%u dwell=%lums hostDur=%lu joinDur=%lu",
                  ctx.isHost ? 1 : 0, ax_, ay_, az_, haveAnchor_ ? 1 : 0, HOP,
                  (unsigned)HOPS, HOP_DWELL_MS, HOST_DURATION_MS, JOIN_DURATION_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO CAMP no leader resolved (empty squad?)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;

        if (haveAnchor_ && evidenceDue(ctx.elapsedMs)) {
            Character* ld = engine::leader(ctx.gw);
            if (!ctx.isHost) {
                // JOIN: hop the leader one HOP further out every HOP_DWELL_MS to
                // drive mint/zone churn; short walk legs inside the dwell keep it
                // a live, moving subject.
                unsigned int wantHops = (unsigned int)(ctx.elapsedMs / HOP_DWELL_MS);
                if (wantHops > HOPS) wantHops = HOPS;
                if (ld) {
                    if (wantHops > hopsDone_) {
                        hopsDone_ = wantHops;
                        float hx = ax_ + (float)hopsDone_ * HOP;
                        engine::park(ld, hx, ay_, az_, 0.0f);
                        char b[96];
                        _snprintf(b, sizeof(b) - 1,
                                  "SCENARIO CAMP hop n=%u to=%.0f,%.0f,%.0f",
                                  hopsDone_, hx, ay_, az_);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    } else {
                        float hx = ax_ + (float)hopsDone_ * HOP;
                        bool legB = ((ctx.elapsedMs / 3000) % 2) != 0;
                        engine::orderMoveTo(ld, hx + (legB ? 15.0f : 0.0f), ay_, az_);
                    }
                }
            }
            // Both sides log their leader position (light telemetry; the real
            // gates read the plugin's [spawn]/[drive]/[leave] lines).
            if (ld) {
                float lx = 0, ly = 0, lz = 0;
                engine::readPos(ld, &lx, &ly, &lz);
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO CAMP pos host=%d %.1f,%.1f,%.1f t=%lu",
                          ctx.isHost ? 1 : 0, lx, ly, lz, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        if (ctx.elapsedMs >= dur) {
            // Host: exiting first IS the peer-drop stimulus. Join: survived the
            // drop + churn (the cleanup/no-stale-drive verdict is the oracle's,
            // read from the flushed log).
            passed_ = haveAnchor_;
            return true;
        }
        return false;
    }

private:
    // Host exits FIRST (the peer drop); join outlives it by ~20 s to log the
    // post-leave cleanup while still hopping. Manifest raises Seconds/KillGrace
    // so the 150 s join window survives the runner backstop.
    // ~40 s gap so the transport reliably detects the host drop (ENet peer
    // timeout) and delivers 'peer left' to the join well before the join's own
    // self-exit - the survivor needs a wide window to log its post-leave cleanup.
    static const unsigned long HOST_DURATION_MS = 120000; // host drops here
    static const unsigned long JOIN_DURATION_MS = 160000; // join survives + logs cleanup
    static const unsigned long HOP_DWELL_MS     = 8000;   // per-stop coverage window
    static const unsigned int  HOPS             = 12;     // total legs
    static const float         HOP;                       // leg length (units)

    unsigned int  hopsDone_;
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};
const float CampApproachScenario::HOP = 2800.0f;

// split_far2 - the scenario presence authority exists for.
//
// split_far establishes the CONDITION (two squads held apart, each in a
// populated place) and measures whether the two clients agree about what is
// there. It cannot say anything about WHO SHOULD AUTHOR a region, because
// under fixed host authority there is only ever one answer. This one is built
// around the two claims the presence-authority design makes:
//
//   1. Each client authors the cell it is standing in, and BOTH clients
//      resolve the same owner for the same cell. The scenario cannot read
//      authorityFor() - scenarios have no handle on the Replicator - so it
//      logs its own leader's cell and lets the oracle join that against the
//      `[cell] MAP` dumps in the two logs. Agreement is the assertion.
//   2. The peer looking at a region we author costs no visible pop. That is
//      the reported symptom in its purest form ("NPCs disappear as I get
//      close, then get replaced by the host's"), and `[attn] attach` already
//      counts exactly it: bodies hidden/culled/minted in the window attention
//      arrived.
//
// APPROACH, not teleport. The baked fixture puts each squad in its town, and the
// approach is deliberately short so the run isolates the ARRIVAL - not because
// the distance is unaffordable. (This comment used to claim walking the 5200 u
// between the towns costs ~9 minutes. It does not: run_apart measured ~570-600
// u/s under a 5x vote, so it is ~9 SECONDS, and run_apart's first window was
// sized three minutes too long on the strength of the wrong figure.) So each
// side PARKS its own tab leader
// APPROACH_D out from where the fixture placed it and WALKS it back in. The
// approach is the part that matters: it is when zones stream, when the census
// first covers a region, and when the attach measurement has something to say.
// A run that teleported into position would skip the transient under test.
//
// Then the same phased-camera design split_far uses, for the same reason -
// pointing both cameras at ONE squad is the only configuration in which "these
// two clients disagree about what is here" has a single explanation.
//
//   walk       each -> own      approach + claim settling (dwell is 3 s)
//   own        each -> own      steady state, disjoint attention
//   both_join  both -> join tab the HOST attends a cell the JOIN authors
//   both_host  both -> host tab the JOIN attends a cell the HOST authors
//   back       each -> own      does the peer's population SURVIVE us leaving
//
// Requires KENSHICOOP_CELL_AUTH=1 (manifest DiagEnv). With it off the run
// still completes and still logs, and the oracle reports the flag state, so a
// misconfigured run reads as inconclusive rather than as a pass.
class SplitFar2Scenario : public TimedScenario {
public:
    SplitFar2Scenario()
        : TimedScenario("split_far2", 1000), recvCount_(0),
          haveAnchor_(false), camMs_(0), camPhase_(0xFFFFFFFFu),
          arrived_(false),
          hx_(0), hy_(0), hz_(0), jx_(0), jy_(0), jz_(0),
          tx_(0), ty_(0), tz_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int h = tabLeaderIdx(sq, n, 0);   // host-owned tab
        int j = tabLeaderIdx(sq, n, 1);   // join-owned tab
        float sep = 0.0f;
        if (h >= 0 && j >= 0) {
            haveAnchor_ = true;
            hx_ = sq[h].x; hy_ = sq[h].y; hz_ = sq[h].z;
            jx_ = sq[j].x; jy_ = sq[j].y; jz_ = sq[j].z;
            float dx = jx_ - hx_, dz = jz_ - hz_;
            sep = (float)sqrt((double)(dx * dx + dz * dz));
            // Our own town centre is the walk TARGET; the start-out point is
            // APPROACH_D beyond it, directly AWAY from the peer. Outward so
            // the approach never shrinks the separation the scenario depends
            // on, and so the corridor walked is genuinely un-streamed ground
            // rather than the strip between the two squads.
            tx_ = ctx.isHost ? hx_ : jx_;
            ty_ = ctx.isHost ? hy_ : jy_;
            tz_ = ctx.isHost ? hz_ : jz_;
            float ox = ctx.isHost ? (hx_ - jx_) : (jx_ - hx_);
            float oz = ctx.isHost ? (hz_ - jz_) : (jz_ - hz_);
            float m = (float)sqrt((double)(ox * ox + oz * oz));
            if (m > 1.0f) { ox /= m; oz /= m; } else { ox = 1.0f; oz = 0.0f; }
            // Park the WHOLE owned tab, not just its leader: a squad left
            // behind would keep the region attended and there would be no
            // approach to measure.
            for (unsigned int i = 0; i < n; ++i) {
                if ((int)tabRankOf(sq, n, i) != (ctx.isHost ? 0 : 1)) continue;
                Character* c = engine::resolve(sq[i]);
                if (!c) continue;
                float sx = sq[i].x + ox * APPROACH_D;
                float sz = sq[i].z + oz * APPROACH_D;
                engine::park(c, sx, sq[i].y, sz, 0.0f);
            }
        }
        int cx = 0, cz = 0;
        int haveCell = (haveAnchor_ && engine::cellAt(ctx.gw, tx_, tz_, &cx, &cz)) ? 1 : 0;
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SPLITFAR2 start side=%s have=%d sep=%.0f "
                  "town=%.0f,%.0f,%.0f cell=%d(%d,%d) approach=%.0f walkMs=%lu "
                  "phaseMs=%lu",
                  ctx.isHost ? "host" : "join", haveAnchor_ ? 1 : 0, sep,
                  tx_, ty_, tz_, haveCell, cx, cz, APPROACH_D,
                  (unsigned long)WALK_MS, (unsigned long)PHASE_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO SPLITFAR2 needs a 2-tab save (rank-0/rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (!haveAnchor_) {
            if (ctx.elapsedMs >= dur) { passed_ = false; return true; }
            return false;
        }

        if (evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int h = tabLeaderIdx(sq, n, 0);
            int j = tabLeaderIdx(sq, n, 1);
            unsigned int ph = phaseOf(ctx.elapsedMs);

            // ---- camera ------------------------------------------------------
            int tgt;
            if      (ph == 2) tgt = j;
            else if (ph == 3) tgt = h;
            else              tgt = ctx.isHost ? h : j;
            if (tgt >= 0 && (camMs_ == 0 || ph != camPhase_ ||
                             (ctx.elapsedMs - camMs_) >= CAM_REFOCUS_MS)) {
                if (ph != camPhase_) {
                    char pb[160];
                    _snprintf(pb, sizeof(pb) - 1,
                              "SCENARIO SPLITFAR2 phase side=%s phase=%s watching=%s",
                              ctx.isHost ? "host" : "join", phaseName(ph),
                              (tgt == h) ? "host" : "join");
                    pb[sizeof(pb) - 1] = '\0'; coop::logLine(pb);
                }
                camMs_ = ctx.elapsedMs; camPhase_ = ph;
                Character* tc = engine::resolve(sq[tgt]);
                if (tc) engine::cameraFocusOn(ctx.gw, tc);
            }

            // ---- locomotion: approach during walk, hold after ----------------
            int own = ctx.isHost ? h : j;
            if (own >= 0) {
                Character* oc = engine::resolve(sq[own]);
                float dx = sq[own].x - tx_, dz = sq[own].z - tz_;
                float d = (float)sqrt((double)(dx * dx + dz * dz));
                if (oc) {
                    if (d > ARRIVE_R) {
                        engine::orderMoveTo(oc, tx_, ty_, tz_);
                    } else {
                        if (!arrived_) {
                            arrived_ = true;
                            char ab[160];
                            _snprintf(ab, sizeof(ab) - 1,
                                      "SCENARIO SPLITFAR2 arrived side=%s atMs=%lu d=%.0f",
                                      ctx.isHost ? "host" : "join",
                                      ctx.elapsedMs, d);
                            ab[sizeof(ab) - 1] = '\0'; coop::logLine(ab);
                        }
                        // Keep the body a live subject rather than a statue,
                        // the travel_parity precedent - but on a short leg
                        // that never leaves the town it just walked into.
                        bool legB = ((ctx.elapsedMs / 4000) % 2) != 0;
                        engine::orderMoveTo(oc, tx_ + (legB ? IDLE_LEG : 0.0f),
                                            ty_, tz_);
                    }
                }
            }

            if (ctx.isHost) {
                if (h >= 0) logScenarioEntity("MEMBER", sq[h]);
                if (j >= 0) { logScenarioEntity("RECV", sq[j]); ++recvCount_; }
            } else {
                if (j >= 0) logScenarioEntity("MEMBER", sq[j]);
                if (h >= 0) { logScenarioEntity("RECV", sq[h]); ++recvCount_; }
            }

            // ---- the measurement row -----------------------------------------
            // Per side, per sample: where each tab is, which CELL it is in,
            // how many bodies WE can see around each, and whether our engine
            // has the ground loaded there. The oracle pairs host and join rows
            // by phase and asks whether the two views of the same region agree
            // - and joins them against `[cell] MAP` to check that they also
            // agree about who authors it.
            if (h >= 0 && j >= 0) {
                float dx = sq[j].x - sq[h].x, dz = sq[j].z - sq[h].z;
                float sep = (float)sqrt((double)(dx * dx + dz * dz));
                int hcx = 0, hcz = 0, jcx = 0, jcz = 0;
                int hc = engine::cellAt(ctx.gw, sq[h].x, sq[h].z, &hcx, &hcz) ? 1 : 0;
                int jc = engine::cellAt(ctx.gw, sq[j].x, sq[j].z, &jcx, &jcz) ? 1 : 0;
                unsigned int popH = engine::countNpcsNear(ctx.gw, sq[h].x, sq[h].y,
                                                          sq[h].z, PROBE_R);
                unsigned int popJ = engine::countNpcsNear(ctx.gw, sq[j].x, sq[j].y,
                                                          sq[j].z, PROBE_R);
                int zH = engine::isZoneLoadedAt(ctx.gw, sq[h].x, sq[h].y, sq[h].z) ? 1 : 0;
                int zJ = engine::isZoneLoadedAt(ctx.gw, sq[j].x, sq[j].y, sq[j].z) ? 1 : 0;
                char b[288];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPLITFAR2 side=%s phase=%s sep=%.0f "
                          "hostCell=%d(%d,%d) joinCell=%d(%d,%d) "
                          "popHost=%u popJoin=%u zHost=%d zJoin=%d arrived=%d",
                          ctx.isHost ? "host" : "join", phaseName(ph), sep,
                          hc, hcx, hcz, jc, jcx, jcz, popH, popJ, zH, zJ,
                          arrived_ ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        if (ctx.elapsedMs >= dur) {
            // As split_far: the scenario passes when it PRODUCED the evidence.
            // Whether authority followed presence is a judgement over the rows
            // and belongs to the oracle.
            passed_ = haveAnchor_ && recvCount_ >= 1;
            return true;
        }
        return false;
    }

private:
    // Phase 0 is the approach; 1..4 are the camera phases. Both sides derive
    // it from the same armed clock, so the swaps happen together.
    static unsigned int phaseOf(unsigned long ms) {
        if (ms < WALK_MS) return 0;
        unsigned int p = 1 + (unsigned int)((ms - WALK_MS) / PHASE_MS);
        return (p > 4) ? 4 : p;
    }
    static const char* phaseName(unsigned int p) {
        return (p == 0) ? "walk"      : (p == 1) ? "own"
             : (p == 2) ? "both_join" : (p == 3) ? "both_host" : "back";
    }

    static const unsigned long WALK_MS          = 60000;  // approach window
    static const unsigned long PHASE_MS         = 40000;  // per camera phase
    static const unsigned long JOIN_DURATION_MS = 220000; // walk + 4 phases
    static const unsigned long HOST_DURATION_MS = 230000; // outlive the join
    static const unsigned long CAM_REFOCUS_MS   = 5000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         APPROACH_D;  // start-out distance from the town
    static const float         ARRIVE_R;    // "we are in the town"
    static const float         IDLE_LEG;    // post-arrival shuffle
    static const float         PROBE_R;     // population probe sphere

    unsigned int  recvCount_;
    bool          haveAnchor_;
    unsigned long camMs_;
    unsigned int  camPhase_;
    bool          arrived_;
    float         hx_, hy_, hz_;   // host tab, as the fixture baked it
    float         jx_, jy_, jz_;   // join tab, as the fixture baked it
    float         tx_, ty_, tz_;   // OUR town centre = the walk target
};
// 600 u: far enough that the destination is outside the ~200 u stream bubble
// and well outside render range, so the walk really does stream the town in,
// but reachable inside WALK_MS at Kenshi's locomotion speed.
const float SplitFar2Scenario::APPROACH_D = 600.0f;
const float SplitFar2Scenario::ARRIVE_R   = 60.0f;
const float SplitFar2Scenario::IDLE_LEG   = 15.0f;
// Matches split_far: what governs whether the two clients can disagree about a
// body is the 2000 u census reach, so the probe measures inside that shell.
const float SplitFar2Scenario::PROBE_R    = 1800.0f;

// run_apart: the pair starts TOGETHER, runs ~160 k u each on foot at 5x in
// OPPOSITE directions, and ends ~138,500 u apart - then the camera phases ask
// what the two clients make of that.
//
// split_far2 answers "who authors this region" from a fixture that already has
// the squads 5200 u apart, and walks only the last 600 u. That is deliberate -
// it isolates the arrival transient - but it means the run never crosses ground,
// and crossing ground is where the reported symptoms live: zones streaming in
// under a moving anchor, cells being claimed and vacated one after another, and
// the census first covering regions neither client has ever attended. Watching
// split_far2, the squads barely move.
//
// So this one starts from runfar1, where both tabs stand together at about
// -50879,3676, and sends the host north to 11470,65362 while the join goes south
// to 35914,-70928. That is sixty-nine times the 2000 u census reach between them,
// well outside anything either engine has streamed, and each squad has crossed
// most of the map to get there - the configuration the field reports come from
// and the one no existing scenario produces.
//
// Both squads run their OWN route, and both run the whole way. An earlier
// revision convoyed them and stopped the host half way, on the theory that half
// a party cannot survive bandit country alone - which was true of the route it
// was then using. These routes are a recording of a human doing exactly this,
// both squads separately, and both arrived, so the convoy is not needed and the
// separation is a real ~138 k rather than the 48 k a shared corridor allowed.
//
// Speed sync arbitrates min(host, join), so BOTH sides must vote 5x or the pair
// runs at whatever the quieter side asked for - which is why this votes on each
// side rather than only on the host, and re-votes.
//
// That arbitration also pins the sim to 1x while either squad fights, which for
// this scenario is the wrong answer to the right question: we are fleeing the
// encounter, not resolving it. The manifest therefore clears
// KENSHICOOP_SPEED_COMBAT_CAP, and the stall handler below skips a waypoint we
// have stopped closing on, so a fight costs a detour instead of the run.
//
// How fast that actually is, measured rather than assumed: ~570-600 u/s of real
// time under the 5x vote, sampled per second off the tab leader's own position,
// walk clip playing and the path curving round obstacles. The note in split_far2
// that 5200 u costs ~9 minutes is wrong by about sixty times - it is ~9 seconds.
// Worth knowing before sizing any window on this, as its first run showed.
//
// A ~160 k path is therefore ~280 s if nothing interrupts. The recording took
// 480 s, a human at the wheel, stationary 41% of the time (fights, orders,
// looking around) and with the combat cap still on. WALK_MS below allows 420 s,
// between the two.
struct RunWp { float x, z; };
// The two routes are RECORDED, at 1 Hz, from a human driving both squads apart
// (KENSHICOOP_TRACK_MOVE, session 20260804_114911), then decimated to ~2000 u
// legs. They are not a straight line and must not be replaced by one.
//
// That is the whole lesson of the three runs before this. The first tried
// split_far2's towns and separated the pair by 6400 u, no further than
// split_far2 already does. The second sent the join cross-map alone and it was
// knocked out 12,200 u in (blood 20 of 117, a limb at 1.7%) - half a party
// cannot cross bandit country, and no amount of re-ordering moves an
// unconscious body. The third walked between the CELL CLAIMS of an earlier
// session, which fire once per 4608 u cell, and wedged both squads at
// -48980,-5670: healthy, idle, out of combat, no path. The reason is in the
// numbers here - the host's recorded path is 164,532 u long to cover 88,242 u
// of displacement, so a straight line between two points a human reached is
// very often through something they walked around, and the router answers by
// not moving at all.
//
// Y is not stored. Each order reuses the body's CURRENT y and lets the engine
// ground the path, which is what keeps a recorded route valid on uneven terrain.
const RunWp HOST_ROUTE[] = {
    { -50879.0f,   3676.0f },    { -50590.0f,   3651.0f },    { -50517.0f,   6158.0f },
    { -51565.0f,   8192.0f },    { -52486.0f,  10021.0f },    { -53212.0f,  12378.0f },
    { -52990.0f,  14737.0f },    { -53785.0f,  17179.0f },    { -54589.0f,  18695.0f },
    { -55734.0f,  20625.0f },    { -55201.0f,  22672.0f },    { -56739.0f,  23085.0f },
    { -56618.0f,  24294.0f },    { -54766.0f,  25344.0f },    { -55482.0f,  27637.0f },
    { -56329.0f,  29431.0f },    { -56279.0f,  31796.0f },    { -55776.0f,  34298.0f },
    { -56683.0f,  36432.0f },    { -56588.0f,  38110.0f },    { -56916.0f,  40470.0f },
    { -56858.0f,  42035.0f },    { -57848.0f,  43802.0f },    { -58258.0f,  45708.0f },
    { -59179.0f,  47539.0f },    { -60246.0f,  49396.0f },    { -59636.0f,  51328.0f },
    { -59889.0f,  53805.0f },    { -60587.0f,  55667.0f },    { -61128.0f,  57691.0f },
    { -60865.0f,  59763.0f },    { -61011.0f,  61943.0f },    { -61081.0f,  64298.0f },
    { -61219.0f,  66062.0f },    { -59046.0f,  67204.0f },    { -57459.0f,  66394.0f },
    { -56040.0f,  64766.0f },    { -54110.0f,  64154.0f },    { -52102.0f,  63899.0f },
    { -50139.0f,  63294.0f },    { -48253.0f,  63620.0f },    { -46517.0f,  62741.0f },
    { -44633.0f,  62860.0f },    { -44379.0f,  62390.0f },    { -44999.0f,  62850.0f },
    { -43913.0f,  63646.0f },    { -41551.0f,  63038.0f },    { -39159.0f,  62084.0f },
    { -37189.0f,  61580.0f },    { -34800.0f,  61604.0f },    { -32287.0f,  61297.0f },
    { -29807.0f,  61025.0f },    { -27348.0f,  60959.0f },    { -24937.0f,  60336.0f },
    { -22889.0f,  60378.0f },    { -20607.0f,  59512.0f },    { -18501.0f,  59962.0f },
    { -16303.0f,  60138.0f },    { -13997.0f,  60023.0f },    { -11675.0f,  59988.0f },
    {  -9287.0f,  59204.0f },    {  -6975.0f,  58092.0f },    {  -4897.0f,  57876.0f },
    {  -2579.0f,  57129.0f },    {    -49.0f,  56597.0f },    {   1538.0f,  57794.0f },
    {   3193.0f,  59737.0f },    {   5013.0f,  61478.0f },    {   6874.0f,  62988.0f },
    {   9212.0f,  64093.0f },    {  11470.0f,  65362.0f }
};
const RunWp JOIN_ROUTE[] = {
    { -50914.0f,   3688.0f },    { -50237.0f,   1556.0f },    { -50129.0f,   -837.0f },
    { -49405.0f,  -2713.0f },    { -49271.0f,  -5142.0f },    { -49018.0f,  -7306.0f },
    { -47469.0f,  -8804.0f },    { -45246.0f,  -9338.0f },    { -43493.0f, -10962.0f },
    { -41302.0f, -11722.0f },    { -40318.0f, -13829.0f },    { -38060.0f, -14635.0f },
    { -35749.0f, -14511.0f },    { -34715.0f, -16121.0f },    { -33524.0f, -18276.0f },
    { -32893.0f, -20457.0f },    { -31287.0f, -21735.0f },    { -28788.0f, -21977.0f },
    { -27090.0f, -23229.0f },    { -24701.0f, -23950.0f },    { -22331.0f, -23430.0f },
    { -20452.0f, -24991.0f },    { -19729.0f, -27351.0f },    { -19383.0f, -29733.0f },
    { -19976.0f, -32037.0f },    { -19101.0f, -34479.0f },    { -17586.0f, -36526.0f },
    { -17523.0f, -38906.0f },    { -16766.0f, -41079.0f },    { -16424.0f, -43367.0f },
    { -16918.0f, -45909.0f },    { -16236.0f, -47523.0f },    { -13830.0f, -46874.0f },
    { -11399.0f, -47303.0f },    {  -8998.0f, -47316.0f },    {  -6463.0f, -47373.0f },
    {  -4470.0f, -48953.0f },    {  -2480.0f, -50177.0f },    {   -569.0f, -51567.0f },
    {   1749.0f, -52391.0f },    {   3815.0f, -50816.0f },    {   5609.0f, -49063.0f },
    {   7998.0f, -48317.0f },    {  10408.0f, -47785.0f },    {  12416.0f, -48423.0f },
    {  14532.0f, -49883.0f },    {  15743.0f, -52116.0f },    {  17349.0f, -53404.0f },
    {  18275.0f, -55442.0f },    {  19532.0f, -57154.0f },    {  19987.0f, -59255.0f },
    {  20061.0f, -61108.0f },    {  21457.0f, -62381.0f },    {  23524.0f, -63109.0f },
    {  25921.0f, -62114.0f },    {  28253.0f, -61757.0f },    {  29182.0f, -64136.0f },
    {  30827.0f, -65560.0f },    {  33058.0f, -66768.0f },    {  34241.0f, -69015.0f },
    {  35914.0f, -70928.0f }
};

class RunApartScenario : public TimedScenario {
public:
    RunApartScenario()
        : TimedScenario("run_apart", 1000), recvCount_(0),
          haveAnchor_(false), camMs_(0), camPhase_(0xFFFFFFFFu),
          arrived_(false), arriveMs_(0), speedMs_(0),
          route_(0), nRoute_(0), wp_(0), travelled_(0.0f), maxStep_(0.0f),
          lastX_(0), lastZ_(0), haveLast_(false),
          bestD_(1.0e9f), stallMs_(0), nStall_(0),
          sx_(0), sy_(0), sz_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        route_  = ctx.isHost ? HOST_ROUTE : JOIN_ROUTE;
        nRoute_ = ctx.isHost
                    ? (unsigned int)(sizeof(HOST_ROUTE) / sizeof(HOST_ROUTE[0]))
                    : (unsigned int)(sizeof(JOIN_ROUTE) / sizeof(JOIN_ROUTE[0]));
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int h = tabLeaderIdx(sq, n, 0);   // host-owned tab
        int j = tabLeaderIdx(sq, n, 1);   // join-owned tab
        float sep = 0.0f;
        if (h >= 0 && j >= 0) {
            haveAnchor_ = true;
            int own = ctx.isHost ? h : j;
            sx_ = sq[own].x; sy_ = sq[own].y; sz_ = sq[own].z;
            lastX_ = sx_; lastZ_ = sz_; haveLast_ = true;
            float dx = sq[j].x - sq[h].x, dz = sq[j].z - sq[h].z;
            sep = (float)sqrt((double)(dx * dx + dz * dz));
        }
        voteSpeed(ctx, 0);
        const RunWp& last = route_[nRoute_ - 1];
        int cx = 0, cz = 0;
        int haveCell = (haveAnchor_ &&
                        engine::cellAt(ctx.gw, last.x, last.z, &cx, &cz)) ? 1 : 0;
        char b[320];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO RUNAPART start side=%s have=%d sep=%.0f "
                  "from=%.0f,%.0f,%.0f dest=%.0f,%.0f cell=%d(%d,%d) "
                  "wps=%u leg=%.0f straight=%.0f speed=%.1f walkMs=%lu phaseMs=%lu",
                  ctx.isHost ? "host" : "join", haveAnchor_ ? 1 : 0, sep,
                  sx_, sy_, sz_, last.x, last.z, haveCell, cx, cz,
                  nRoute_, routeLength(), straightLength(), SPEED_MULT,
                  (unsigned long)WALK_MS, (unsigned long)PHASE_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO RUNAPART needs a 2-tab save (rank-0/rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (!haveAnchor_) {
            if (ctx.elapsedMs >= dur) { passed_ = false; return true; }
            return false;
        }
        // Re-vote periodically. A speed vote is a UI-visible click, and anything
        // that clicks after us (the replicator's arbitration, a phase change,
        // the engine's own combat handling) would otherwise quietly leave the
        // pair walking at 1x for the rest of a leg measured in thousands of
        // units - the run would simply not finish, and the log would not say why.
        voteSpeed(ctx, ctx.elapsedMs);

        if (evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int h = tabLeaderIdx(sq, n, 0);
            int j = tabLeaderIdx(sq, n, 1);
            unsigned int ph = phaseOf(ctx.elapsedMs);

            // ---- camera: same phase plan as split_far2 -----------------------
            int tgt;
            if      (ph == 2) tgt = j;
            else if (ph == 3) tgt = h;
            else              tgt = ctx.isHost ? h : j;
            if (tgt >= 0 && (camMs_ == 0 || ph != camPhase_ ||
                             (ctx.elapsedMs - camMs_) >= CAM_REFOCUS_MS)) {
                if (ph != camPhase_) {
                    char pb[160];
                    _snprintf(pb, sizeof(pb) - 1,
                              "SCENARIO RUNAPART phase side=%s phase=%s watching=%s",
                              ctx.isHost ? "host" : "join", phaseName(ph),
                              (tgt == h) ? "host" : "join");
                    pb[sizeof(pb) - 1] = '\0'; coop::logLine(pb);
                }
                camMs_ = ctx.elapsedMs; camPhase_ = ph;
                Character* tc = engine::resolve(sq[tgt]);
                if (tc) engine::cameraFocusOn(ctx.gw, tc);
            }

            // ---- locomotion --------------------------------------------------
            // Ordered in EVERY phase, not just the walk window, because the leg
            // is long enough that a run which loses its 5x cannot be assumed to
            // finish on schedule. A body that has arrived just shuffles.
            int own = ctx.isHost ? h : j;
            float d = -1.0f;
            if (own >= 0) {
                // Path length actually covered, summed per sample. A straight
                // line from the start would badly understate a route that turns
                // twenty-nine times, and it is the covered distance - not the
                // displacement - that says whether the squad ran.
                if (haveLast_) {
                    float px = sq[own].x - lastX_, pz = sq[own].z - lastZ_;
                    float step = (float)sqrt((double)(px * px + pz * pz));
                    travelled_ += step;
                    if (step > maxStep_) maxStep_ = step;
                }
                lastX_ = sq[own].x; lastZ_ = sq[own].z; haveLast_ = true;

                // Walk the route. A point counts as reached generously, since these
                // are 1 Hz samples of somebody walking, not places to stand.
                const unsigned int wpWas = wp_;
                while (wp_ + 1 < nRoute_ && wpDist(sq[own]) <= WAYPOINT_R) ++wp_;
                d = wpDist(sq[own]);
                // A new waypoint is a new distance, ~2000 u of it. Carrying the
                // old best across the change would read as "not closing" and trip
                // the stall clock on a squad that is running perfectly.
                if (wp_ != wpWas) { bestD_ = d; stallMs_ = ctx.elapsedMs; }

                // Not closing on it for 20 s? Skip it. Safe here in a way it was
                // not on the sparse route: the next point is ~2000 u further along
                // ground a human covered, so skipping costs one leg, where skipping
                // a cell-crossing point moved the target 5 k further into terrain
                // with no path. A fight is the usual cause and running on is the
                // right answer to it. Capped, so a genuinely stuck squad ends the
                // run reporting stalls rather than teleporting up the route.
                // Only while still running. After arrival the squad shuffles in
                // place on purpose, which never closes distance, and the first run
                // duly reported four "stalls" per side for a route both had
                // finished - a stall count is a fight count and must stay readable.
                if (d < bestD_ - STALL_EPS) { bestD_ = d; stallMs_ = ctx.elapsedMs; }
                else if (!arrived_ && ctx.elapsedMs - stallMs_ >= STALL_MS) {
                    char sb[208];
                    _snprintf(sb, sizeof(sb) - 1,
                              "SCENARIO RUNAPART stall side=%s wp=%u/%u d=%.0f "
                              "forMs=%lu n=%u%s",
                              ctx.isHost ? "host" : "join", wp_ + 1, nRoute_, d,
                              ctx.elapsedMs - stallMs_, nStall_ + 1,
                              (nStall_ < MAX_STALL_SKIPS && wp_ + 1 < nRoute_)
                                  ? " - skipping it" : " - holding");
                    sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
                    if (nStall_ < MAX_STALL_SKIPS && wp_ + 1 < nRoute_) ++wp_;
                    ++nStall_;
                    d = wpDist(sq[own]);
                    bestD_ = d; stallMs_ = ctx.elapsedMs;
                }
                const RunWp& dest = route_[nRoute_ - 1];
                bool lastWp = (wp_ + 1 == nRoute_);
                float dd = destDist(sq[own]);
                if (!(lastWp && d <= ARRIVE_R) && !arrived_) {
                    // Order the next RECORDED point, ~2000 u away along ground a
                    // human covered. Not the far destination: asking for one order
                    // across 160 k u leaves the whole crossing to a single routing
                    // decision, and this route exists precisely because that
                    // decision is the thing that failed.
                    //
                    // The WHOLE owned tab, not just the leader: an unordered member
                    // stays put, keeps its region attended, and ends the run with
                    // the squad strung across the map for reasons that have nothing
                    // to do with authority.
                    for (unsigned int i = 0; i < n; ++i) {
                        if ((int)tabRankOf(sq, n, i) != (ctx.isHost ? 0 : 1)) continue;
                        Character* mc = engine::resolve(sq[i]);
                        if (mc) engine::orderMoveTo(mc, route_[wp_].x, sq[i].y,
                                                    route_[wp_].z);
                    }
                } else {
                    if (!arrived_) {
                        arrived_ = true;
                        arriveMs_ = ctx.elapsedMs;
                        char ab[192];
                        _snprintf(ab, sizeof(ab) - 1,
                                  "SCENARIO RUNAPART arrived side=%s atMs=%lu d=%.0f "
                                  "travelled=%.0f maxStep=%.0f",
                                  ctx.isHost ? "host" : "join", ctx.elapsedMs,
                                  dd, travelled_, maxStep_);
                        ab[sizeof(ab) - 1] = '\0'; coop::logLine(ab);
                    }
                    Character* oc = engine::resolve(sq[own]);
                    bool legB = ((ctx.elapsedMs / 4000) % 2) != 0;
                    if (oc) engine::orderMoveTo(oc, dest.x + (legB ? IDLE_LEG : 0.0f),
                                                sq[own].y, dest.z);
                }
            }

            if (ctx.isHost) {
                if (h >= 0) logScenarioEntity("MEMBER", sq[h]);
                if (j >= 0) { logScenarioEntity("RECV", sq[j]); ++recvCount_; }
            } else {
                if (j >= 0) logScenarioEntity("MEMBER", sq[j]);
                if (h >= 0) { logScenarioEntity("RECV", sq[h]); ++recvCount_; }
            }

            // ---- the measurement row -----------------------------------------
            // split_far2's row plus the two numbers this scenario exists to
            // produce: how far our own tab has COME, and how far it still has to
            // go. Without them a run that never moved and a run that finished
            // look the same in the log.
            if (h >= 0 && j >= 0) {
                float dx = sq[j].x - sq[h].x, dz = sq[j].z - sq[h].z;
                float sep = (float)sqrt((double)(dx * dx + dz * dz));
                int hcx = 0, hcz = 0, jcx = 0, jcz = 0;
                int hc = engine::cellAt(ctx.gw, sq[h].x, sq[h].z, &hcx, &hcz) ? 1 : 0;
                int jc = engine::cellAt(ctx.gw, sq[j].x, sq[j].z, &jcx, &jcz) ? 1 : 0;
                unsigned int popH = engine::countNpcsNear(ctx.gw, sq[h].x, sq[h].y,
                                                          sq[h].z, PROBE_R);
                unsigned int popJ = engine::countNpcsNear(ctx.gw, sq[j].x, sq[j].y,
                                                          sq[j].z, PROBE_R);
                int zH = engine::isZoneLoadedAt(ctx.gw, sq[h].x, sq[h].y, sq[h].z) ? 1 : 0;
                int zJ = engine::isZoneLoadedAt(ctx.gw, sq[j].x, sq[j].y, sq[j].z) ? 1 : 0;
                float mult = 0.0f; bool paused = false;
                engine::readGameSpeed(ctx.gw, &mult, &paused);
                char b[384];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO RUNAPART side=%s phase=%s sep=%.0f "
                          "hostCell=%d(%d,%d) joinCell=%d(%d,%d) "
                          "popHost=%u popJoin=%u zHost=%d zJoin=%d arrived=%d "
                          "wp=%u/%u togo=%.0f travelled=%.0f stalls=%u speed=%.1f",
                          ctx.isHost ? "host" : "join", phaseName(ph), sep,
                          hc, hcx, hcz, jc, jcx, jcz, popH, popJ, zH, zJ,
                          arrived_ ? 1 : 0, wp_ + 1, nRoute_, d, travelled_,
                          nStall_, mult);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        if (ctx.elapsedMs >= dur) {
            passed_ = haveAnchor_ && recvCount_ >= 1;
            return true;
        }
        return false;
    }

private:
    // One vote per SPEED_VOTE_MS, and always one at start (ms == 0).
    void voteSpeed(const ScenarioContext& ctx, unsigned long ms) {
        if (ms != 0 && (ms - speedMs_) < SPEED_VOTE_MS) return;
        speedMs_ = ms;
        engine::writeGameSpeed(ctx.gw, SPEED_MULT, false);
    }
    float wpDist(const EntityState& e) const {
        float dx = e.x - route_[wp_].x, dz = e.z - route_[wp_].z;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    float destDist(const EntityState& e) const {
        float dx = e.x - route_[nRoute_ - 1].x, dz = e.z - route_[nRoute_ - 1].z;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    // Start -> wp0 -> wp1 -> ... : what the squad has to cover, which is what
    // 'travelled' is measured against.
    float routeLength() const {
        float total = 0.0f, px = sx_, pz = sz_;
        for (unsigned int i = 0; i < nRoute_; ++i) {
            float dx = route_[i].x - px, dz = route_[i].z - pz;
            total += (float)sqrt((double)(dx * dx + dz * dz));
            px = route_[i].x; pz = route_[i].z;
        }
        return total;
    }
    float straightLength() const {
        float dx = route_[nRoute_ - 1].x - sx_, dz = route_[nRoute_ - 1].z - sz_;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    static unsigned int phaseOf(unsigned long ms) {
        if (ms < WALK_MS) return 0;
        unsigned int p = 1 + (unsigned int)((ms - WALK_MS) / PHASE_MS);
        return (p > 4) ? 4 : p;
    }
    static const char* phaseName(unsigned int p) {
        return (p == 0) ? "run"       : (p == 1) ? "own"
             : (p == 2) ? "both_join" : (p == 3) ? "both_host" : "back";
    }

    // ~160 k u of route at the measured ~570 u/s is ~280 s; the human recording
    // of these same two routes took 480 s while stopping to fight and look
    // around. 420 s sits between them, and since the locomotion order runs in
    // EVERY phase, a slow run finishes late rather than stranding the squad half
    // way - it just does its camera phases while still walking.
    static const unsigned long WALK_MS          = 420000;
    static const unsigned long PHASE_MS         = 40000;
    static const unsigned long JOIN_DURATION_MS = 580000; // run + 4 phases
    static const unsigned long HOST_DURATION_MS = 590000; // outlive the join
    static const unsigned long CAM_REFOCUS_MS   = 5000;
    static const unsigned long SPEED_VOTE_MS    = 10000;
    // 20 s without getting closer is a fight or a wall, not slow going: at the
    // measured rate a clear run closes 400 u in under a second.
    static const unsigned long STALL_MS         = 20000;
    // Skipping is for getting past a fight, not for covering the route. Twenty
    // skips is ~40 k u of the recorded path at most; beyond that the run is not
    // running and should say so instead of shuffling to the end.
    static const unsigned int  MAX_STALL_SKIPS   = 20;
    static const unsigned int  MAX_SQUAD        = 32;
    static const float SPEED_MULT;
    static const float STALL_EPS;      // progress worth resetting the stall clock
    static const float WAYPOINT_R;
    static const float ARRIVE_R;
    static const float IDLE_LEG;
    static const float PROBE_R;

    unsigned int  recvCount_;
    bool          haveAnchor_;
    unsigned long camMs_;
    unsigned int  camPhase_;
    bool          arrived_;
    unsigned long arriveMs_;
    unsigned long speedMs_;
    const RunWp*  route_;
    unsigned int  nRoute_;
    unsigned int  wp_;             // index of the waypoint we are heading for
    float         travelled_;      // summed path length, not displacement
    float         maxStep_;        // biggest single-sample jump (a snap shows here)
    float         lastX_, lastZ_;
    bool          haveLast_;
    float         bestD_;          // closest we have been to the current waypoint
    unsigned long stallMs_;        // when we last got closer to it
    unsigned int  nStall_;         // stalls seen (a fight count, in effect)
    float         sx_, sy_, sz_;   // where our tab started (travel baseline)
};
const float RunApartScenario::SPEED_MULT = 5.0f;
// 50 u of closing counts as progress. Below that is combat jostle, which should
// not keep resetting the stall clock and hiding a squad that is going nowhere.
const float RunApartScenario::STALL_EPS = 50.0f;
// 400 u to call a waypoint reached. At ~570 u/s a sample can overshoot one by
// several hundred units, so a tight radius would leave the squad circling back to
// touch a point that was only ever a breadcrumb.
const float RunApartScenario::WAYPOINT_R = 400.0f;
// The LAST point is a real destination, so it gets a real radius: 150 u, loose
// enough for a squad arriving strung out behind its leader.
const float RunApartScenario::ARRIVE_R = 150.0f;
const float RunApartScenario::IDLE_LEG = 15.0f;
const float RunApartScenario::PROBE_R  = 1800.0f;

// town_arrive (zone-load population parity, 2026-08-06): walk BOTH squads into a
// town whose zone has NEVER been loaded in this save, and judge what the join
// sees once it gets there.
//
// Why this needs its own scenario, and why loading a save inside the town does
// not test it: a town's population is GENERATED when its zone streams in. Load a
// save that was written in the town and both clients read the same baked bodies,
// so the hands match and the census resolves - measured in Bad Teeth, 78% of the
// host's census rows resolve to a local body and 9% of the join's population is
// suppressed. Walk into that same town instead and each engine generates its own
// population: same spawn points, different hands. The join then cannot resolve
// the host's census, mints proxies for all of it, and suppresses its own
// natively-spawned bodies as unclaimed - the same town walked into measured 4%
// resolved and 68% suppressed, with 225 proxy binds and 264 census-missing rows
// (session 20260806_1224). On screen that is a town of NPCs popping in and out,
// which is how it was found.
//
// So the fixture has to be a save that has never SEEN the target town, and the
// bodies have to arrive on foot. Both squads park at a measured point outside it
// - census=0 there, so none of the town is loaded yet, which is the property the
// park point is chosen for - wait for their own zone to stream, then walk in
// under their own locomotion at 5x, which is how a player actually crosses the
// map and which leaves the streaming machinery the least time to keep up.
//
// Navigation is SELF-GUIDING rather than a recorded route: aim HOP_D along the
// straight line to the target, re-aimed every sample, so each routing decision
// is short. run_apart's recorded routes exist because a single order across
// 160 k u fails outright; over the ~6.7 k u of a town approach the router copes,
// and a self-guiding walker retargets to any town by changing two coordinates
// instead of recording a new route. When it stops closing, SIDESTEP: swing the
// aim off the bearing by growing angles, alternating sides, so it walks around
// the obstacle instead of standing in front of it. Progress resets the bearing.
//
// Both the start and the target are env-overridable (KENSHICOOP_TOWN_FROM /
// KENSHICOOP_TOWN_AT, "x,z"), because the gate is about population identity and
// not about this particular town.
class TownArriveScenario : public TimedScenario {
public:
    TownArriveScenario()
        : TimedScenario("town_arrive", 1000), recvCount_(0),
          haveTabs_(false), parked_(0), settled_(false), settleMs_(0),
          arrived_(false), arriveMs_(0), speedMs_(0), camMs_(0),
          fx_(0), fz_(0), tx_(0), tz_(0), groundOk_(false),
          travelled_(0.0f), lastX_(0), lastZ_(0), haveLast_(false),
          hops_(0), bestD_(1.0e9f), stallMs_(0), nSide_(0), bias_(0.0f) {}

    virtual void onStart(const ScenarioContext& ctx) {
        // Measured defaults (session 20260806_1224): the park point is the cell
        // claim one cell short of town, where the join's audit reported census=0
        // and wide=0 - nothing of the town in memory. The target is the camera
        // centre while standing in it.
        fx_ = -35747.0f; fz_ = -14509.0f;
        tx_ = -32899.0f; tz_ = -20539.0f;
        readPt("KENSHICOOP_TOWN_FROM", &fx_, &fz_);
        readPt("KENSHICOOP_TOWN_AT",   &tx_, &tz_);

        voteSpeed(ctx, 0);

        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int h = tabLeaderIdx(sq, n, 0);
        int j = tabLeaderIdx(sq, n, 1);
        haveTabs_ = (h >= 0 && j >= 0);

        // Park the OWN tab only. Each side authors its own tab, so a symmetric
        // park keeps the teleport on the side that owns it and lets the peer's
        // copy converge through the normal channel - one snap, inside the settle
        // window, which the oracle's judged window starts after.
        float gy = 0.0f;
        groundOk_ = engine::terrainHeightAt(fx_, fz_, &gy);
        if (haveTabs_) {
            const int ownRank = ctx.isHost ? 0 : 1;
            for (unsigned int i = 0; i < n; ++i) {
                if (tabRankOf(sq, n, i) != ownRank) continue;
                Character* c = engine::resolve(sq[i]);
                if (!c) continue;
                // Spread along x so they do not land stacked, and offset the
                // join's tab clear of the host's so the two squads arrive
                // together without standing inside each other.
                float px = fx_ + (float)parked_ * 4.0f + (ctx.isHost ? 0.0f : 30.0f);
                float py = groundOk_ ? gy : sq[i].y;
                if (engine::park(c, px, py, fz_, 0.0f)) ++parked_;
            }
            lastX_ = fx_; lastZ_ = fz_; haveLast_ = true;
        }
        bestD_ = straightLength();
        stallMs_ = 0;

        char b[352];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO TOWNARRIVE start side=%s have=%d parked=%u "
                  "from=%.0f,%.0f,%.0f target=%.0f,%.0f straight=%.0f ground=%d "
                  "hop=%.0f speed=%.1f settleMs=%lu holdMs=%lu",
                  ctx.isHost ? "host" : "join", haveTabs_ ? 1 : 0, parked_,
                  fx_, groundOk_ ? gy : 0.0f, fz_, tx_, tz_, straightLength(),
                  groundOk_ ? 1 : 0, HOP_D, SPEED_MULT,
                  (unsigned long)SETTLE_MS, (unsigned long)HOLD_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveTabs_)
            coop::logLine("SCENARIO TOWNARRIVE needs a 2-tab save (rank-0/rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveTabs_) {
            if (ctx.elapsedMs >= HARD_MS) { passed_ = false; return true; }
            return false;
        }
        // Re-vote: anything that clicks after us (the replicator's arbitration, a
        // fight, the engine's own handling) would otherwise leave the pair
        // walking a several-thousand-unit approach at 1x, and the log would not
        // say why the run timed out short of town.
        voteSpeed(ctx, ctx.elapsedMs);

        if (evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int h = tabLeaderIdx(sq, n, 0);
            int j = tabLeaderIdx(sq, n, 1);
            int own = ctx.isHost ? h : j;

            // Watch our own tab throughout. Unlike the split scenarios there is
            // nothing to be learned from looking at the peer here: the bug is in
            // what OUR client renders around OUR characters, and the camera is
            // itself an interest anchor, so pointing it elsewhere would change
            // the thing being measured.
            if (own >= 0 && (camMs_ == 0 || (ctx.elapsedMs - camMs_) >= CAM_REFOCUS_MS)) {
                camMs_ = ctx.elapsedMs;
                Character* oc = engine::resolve(sq[own]);
                if (oc) engine::cameraFocusOn(ctx.gw, oc);
            }

            float d = -1.0f;
            if (own >= 0) {
                if (haveLast_) {
                    float px = sq[own].x - lastX_, pz = sq[own].z - lastZ_;
                    float step = (float)sqrt((double)(px * px + pz * pz));
                    // The park itself is a teleport, not travel.
                    if (step <= MAX_STEP) travelled_ += step;
                }
                lastX_ = sq[own].x; lastZ_ = sq[own].z; haveLast_ = true;
                d = destDist(sq[own]);

                if (!settled_) {
                    // Do not start walking until the ground we were parked on is
                    // actually streamed in: ordering a move through an unloaded
                    // zone is how a squad ends up standing still for a window.
                    bool zone = engine::isZoneLoadedAt(ctx.gw, sq[own].x, sq[own].y,
                                                       sq[own].z);
                    if (zone && ctx.elapsedMs >= SETTLE_MS) {
                        settled_ = true; settleMs_ = ctx.elapsedMs;
                        bestD_ = d; stallMs_ = ctx.elapsedMs;
                        char sb[224];
                        _snprintf(sb, sizeof(sb) - 1,
                                  "SCENARIO TOWNARRIVE settled side=%s atMs=%lu zone=%d "
                                  "pop=%u d=%.0f drift=%.0f",
                                  ctx.isHost ? "host" : "join", ctx.elapsedMs,
                                  zone ? 1 : 0,
                                  engine::countNpcsNear(ctx.gw, sq[own].x, sq[own].y,
                                                        sq[own].z, PROBE_R),
                                  d, parkDrift(sq[own]));
                        sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
                    }
                } else if (!arrived_ && d >= 0.0f && d <= ARRIVE_R) {
                    arrived_ = true; arriveMs_ = ctx.elapsedMs;
                    char ab[224];
                    _snprintf(ab, sizeof(ab) - 1,
                              "SCENARIO TOWNARRIVE arrived side=%s atMs=%lu d=%.0f "
                              "travelled=%.0f straight=%.0f hops=%u sidesteps=%u "
                              "walkMs=%lu",
                              ctx.isHost ? "host" : "join", ctx.elapsedMs, d,
                              travelled_, straightLength(), hops_, nSide_,
                              ctx.elapsedMs - settleMs_);
                    ab[sizeof(ab) - 1] = '\0'; coop::logLine(ab);
                }

                if (settled_ && !arrived_) {
                    trackStall(ctx, d);
                    walkStep(ctx, sq, n, sq[own]);
                } else if (arrived_) {
                    // Shuffle on the spot so the bodies keep a live locomotion
                    // state in town (a frozen squad would not exercise the
                    // streaming this scenario is here to judge) without leaving
                    // the population we are measuring.
                    Character* oc = engine::resolve(sq[own]);
                    bool legB = ((ctx.elapsedMs / 4000) % 2) != 0;
                    if (oc) engine::orderMoveTo(oc, tx_ + (legB ? IDLE_LEG : 0.0f),
                                                sq[own].y, tz_);
                }
            }

            if (ctx.isHost) {
                if (h >= 0) logScenarioEntity("MEMBER", sq[h]);
                if (j >= 0) { logScenarioEntity("RECV", sq[j]); ++recvCount_; }
            } else {
                if (j >= 0) logScenarioEntity("MEMBER", sq[j]);
                if (h >= 0) { logScenarioEntity("RECV", sq[h]); ++recvCount_; }
            }

            // popHost/popJoin are the same two POINTS sampled on both clients, so
            // the pair is comparable across logs - but they come from the sphere
            // query, which is the one that under-reports inside a town, so they
            // are reported and never gated. The gate reads the join's
            // "[audit] exist" enumeration instead.
            if (h >= 0 && j >= 0) {
                float dx = sq[j].x - sq[h].x, dz = sq[j].z - sq[h].z;
                float sep = (float)sqrt((double)(dx * dx + dz * dz));
                int cx = 0, cz = 0;
                int hc = engine::cellAt(ctx.gw, sq[own >= 0 ? own : h].x,
                                        sq[own >= 0 ? own : h].z, &cx, &cz) ? 1 : 0;
                unsigned int popH = engine::countNpcsNear(ctx.gw, sq[h].x, sq[h].y,
                                                          sq[h].z, PROBE_R);
                unsigned int popJ = engine::countNpcsNear(ctx.gw, sq[j].x, sq[j].y,
                                                          sq[j].z, PROBE_R);
                int zH = engine::isZoneLoadedAt(ctx.gw, sq[h].x, sq[h].y, sq[h].z) ? 1 : 0;
                int zJ = engine::isZoneLoadedAt(ctx.gw, sq[j].x, sq[j].y, sq[j].z) ? 1 : 0;
                float mult = 0.0f; bool paused = false;
                engine::readGameSpeed(ctx.gw, &mult, &paused);
                char b[416];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO TOWNARRIVE side=%s phase=%s d=%.0f "
                          "travelled=%.0f popHost=%u popJoin=%u zHost=%d zJoin=%d "
                          "sep=%.0f cell=%d(%d,%d) arrived=%d hops=%u "
                          "sidesteps=%u bias=%.0f speed=%.1f",
                          ctx.isHost ? "host" : "join", phaseName(), d,
                          travelled_, popH, popJ, zH, zJ, sep, hc, cx, cz,
                          arrived_ ? 1 : 0, hops_, nSide_, bias_, mult);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // The judged window is the HOLD after arrival, so the run ends a fixed
        // hold past whenever we got there rather than at a fixed wall time - a
        // slow approach must not eat the measurement. The host outlives the join
        // so the join's disconnect is never the thing that ends the host's run.
        unsigned long extra = ctx.isHost ? HOST_EXTRA_MS : 0;
        if (arrived_) {
            // The hold gets its full window wherever the approach ended. Letting
            // the walk deadline also end an arrived run would silently shorten
            // the only part that is judged, and label a slow approach a timeout.
            if (ctx.elapsedMs >= arriveMs_ + HOLD_MS + extra) {
                passed_ = (recvCount_ >= 1);
                return true;
            }
        } else if (ctx.elapsedMs >= HARD_MS + extra) {
            // Out of time short of town: report it rather than judging a window
            // that never happened.
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO TOWNARRIVE timeout side=%s d=%.0f travelled=%.0f "
                      "hops=%u sidesteps=%u settled=%d",
                      ctx.isHost ? "host" : "join", destDistLast(), travelled_,
                      hops_, nSide_, settled_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            passed_ = false;
            return true;
        }
        return false;
    }

private:
    static void readPt(const char* var, float* x, float* z) {
        const char* v = getenv(var);
        if (v && *v) sscanf(v, "%f,%f", x, z);
    }
    void voteSpeed(const ScenarioContext& ctx, unsigned long ms) {
        if (ms != 0 && (ms - speedMs_) < SPEED_VOTE_MS) return;
        speedMs_ = ms;
        engine::writeGameSpeed(ctx.gw, SPEED_MULT, false);
    }
    float straightLength() const {
        float dx = tx_ - fx_, dz = tz_ - fz_;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    float destDist(const EntityState& e) const {
        float dx = e.x - tx_, dz = e.z - tz_;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    float destDistLast() const {
        if (!haveLast_) return -1.0f;
        float dx = lastX_ - tx_, dz = lastZ_ - tz_;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    // How far the tab leader ended up from the nominal park point. Tens of units
    // are the deliberate spread (4 u per member, 30 u between tabs); hundreds
    // mean the park landed in the air or inside something, and the approach is
    // starting from somewhere other than the measured point.
    float parkDrift(const EntityState& e) const {
        float dx = e.x - fx_, dz = e.z - fz_;
        return (float)sqrt((double)(dx * dx + dz * dz));
    }
    const char* phaseName() const {
        return arrived_ ? "hold" : (settled_ ? "walk" : "settle");
    }

    // Not closing on the town for STALL_MS is an obstacle or a fight, not slow
    // going: at the measured ~570 u/s under a 5x vote a clear approach closes
    // HOP_D in about a second. Answer it by swinging the aim off the bearing,
    // alternating sides and growing the angle, so the squad tries around one
    // side then the other rather than pressing into whatever stopped it.
    void trackStall(const ScenarioContext& ctx, float d) {
        if (d < 0.0f) return;
        if (d < bestD_ - STALL_EPS) {
            bestD_ = d; stallMs_ = ctx.elapsedMs; bias_ = 0.0f;
            return;
        }
        if (ctx.elapsedMs - stallMs_ < STALL_MS) return;
        if (nSide_ >= MAX_SIDESTEPS) {
            // Keep walking straight at it and let the timeout report the truth;
            // silently sidestepping forever would read as a healthy approach.
            stallMs_ = ctx.elapsedMs;
            return;
        }
        ++nSide_;
        float mag = (float)((nSide_ + 1) / 2) * SIDESTEP_DEG;
        if (mag > MAX_BIAS_DEG) mag = MAX_BIAS_DEG;
        bias_ = ((nSide_ % 2) != 0) ? mag : -mag;
        stallMs_ = ctx.elapsedMs;
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO TOWNARRIVE stall side=%s d=%.0f best=%.0f forMs=%lu "
                  "n=%u bias=%.0f - sidestepping",
                  ctx.isHost ? "host" : "join", d, bestD_, (unsigned long)STALL_MS,
                  nSide_, bias_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Aim HOP_D along the (bias-rotated) bearing to town and order the WHOLE
    // owned tab there. Not the far target: one order per sample over a short leg
    // keeps every routing decision small, which is what makes a self-guiding
    // walker viable over an approach. Not the leader alone: an unordered member
    // stays put, keeps its own region attended, and strings the squad out for
    // reasons that have nothing to do with what is being measured.
    void walkStep(const ScenarioContext& ctx, const EntityState* sq,
                  unsigned int n, const EntityState& own) {
        float dx = tx_ - own.x, dz = tz_ - own.z;
        float d = (float)sqrt((double)(dx * dx + dz * dz));
        if (d < 1.0f) return;
        float ux = dx / d, uz = dz / d;
        float r = bias_ * 3.14159265f / 180.0f;
        float cs = (float)cos((double)r), sn = (float)sin((double)r);
        float rx = ux * cs - uz * sn, rz = ux * sn + uz * cs;
        float hop = (d < HOP_D) ? d : HOP_D;
        float ax = own.x + rx * hop, az = own.z + rz * hop;
        const int ownRank = ctx.isHost ? 0 : 1;
        for (unsigned int i = 0; i < n; ++i) {
            if (tabRankOf(sq, n, i) != ownRank) continue;
            Character* mc = engine::resolve(sq[i]);
            if (mc) engine::orderMoveTo(mc, ax, sq[i].y, az);
        }
        ++hops_;
    }

    // A park is a teleport and the settle has to absorb it: the destination zone
    // streams in, the bodies ground, and the peer's copy of the parked tab snaps
    // once. 25 s covers all three at 5x.
    static const unsigned long SETTLE_MS      = 25000;
    // The judged window. At the audit's 5 s cadence this is 24 samples, enough
    // that a median is a median rather than a coin toss.
    static const unsigned long HOLD_MS        = 120000;
    // ~6.7 k u at the measured ~570 u/s is ~12 s of clear walking. 240 s allows
    // an approach that fights, detours and sidesteps most of the way in.
    static const unsigned long HARD_MS        = 240000;
    static const unsigned long HOST_EXTRA_MS  = 10000;
    static const unsigned long SPEED_VOTE_MS  = 10000;
    static const unsigned long CAM_REFOCUS_MS = 5000;
    static const unsigned long STALL_MS       = 15000;
    static const unsigned int  MAX_SIDESTEPS  = 24;
    static const unsigned int  MAX_SQUAD      = 32;
    static const float SPEED_MULT;
    static const float HOP_D;
    static const float ARRIVE_R;
    static const float STALL_EPS;
    static const float SIDESTEP_DEG;
    static const float MAX_BIAS_DEG;
    static const float MAX_STEP;
    static const float IDLE_LEG;
    static const float PROBE_R;

    unsigned int  recvCount_;
    bool          haveTabs_;
    unsigned int  parked_;
    bool          settled_;
    unsigned long settleMs_;
    bool          arrived_;
    unsigned long arriveMs_;
    unsigned long speedMs_;
    unsigned long camMs_;
    float         fx_, fz_;        // measured park point outside the town
    float         tx_, tz_;        // town centre
    bool          groundOk_;
    float         travelled_;
    float         lastX_, lastZ_;
    bool          haveLast_;
    unsigned int  hops_;
    float         bestD_;          // closest we have been to town
    unsigned long stallMs_;
    unsigned int  nSide_;
    float         bias_;           // current aim offset, degrees
};
const float TownArriveScenario::SPEED_MULT = 5.0f;
// Short enough that each routing decision is local, long enough that a 1 Hz
// sample cannot overshoot the aim and leave the squad circling it.
const float TownArriveScenario::HOP_D    = 500.0f;
// The town centre is a destination for a squad arriving strung out behind its
// leader, and being 200 u into a town is being in it.
const float TownArriveScenario::ARRIVE_R = 200.0f;
const float TownArriveScenario::STALL_EPS = 50.0f;
const float TownArriveScenario::SIDESTEP_DEG = 40.0f;
const float TownArriveScenario::MAX_BIAS_DEG = 120.0f;
// Bigger than a sample's worth of running at 5x, so the opening park does not
// land in the travelled total.
const float TownArriveScenario::MAX_STEP  = 1500.0f;
const float TownArriveScenario::IDLE_LEG  = 15.0f;
const float TownArriveScenario::PROBE_R   = 1800.0f;

} // namespace

Scenario* makeMovementScenario(const std::string& name) {
    if (name == "split_far2")   return new SplitFar2Scenario();
    if (name == "run_apart")    return new RunApartScenario();
    if (name == "town_arrive")  return new TownArriveScenario();
    if (name == "leader_move")  return new LeaderMoveScenario();
    if (name == "fast_march")   return new FastMarchScenario();
    if (name == "coop_presence") return new CoopPresenceScenario();
    if (name == "travel_parity") return new TravelParityScenario();
    if (name == "split_interest") return new SplitInterestScenario();
    if (name == "split_far")     return new SplitFarScenario();
    if (name == "camp_approach") return new CampApproachScenario();
    return 0;
}

} // namespace coop
