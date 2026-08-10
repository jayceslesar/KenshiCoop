// WorkPose.h - pose-fixture acceptance policy (pure, zero game/Win32 deps).
//
// When the join reproduces a streamed rest/work pose it resolves the subject hand
// to a LOCAL fixture, then decides whether to trust that fixture or reject it and
// park the body. The two fixture families behave very differently cross-client:
//
//   * SEATS/beds (stools, thrones, beds) are generic, interchangeable props. The
//     same subject HANDLE frequently resolves to a DIFFERENT instance tens of
//     metres away on the join, so issuing the seat goal there walks the body off
//     (then the position-drive teleports it back: the "walk in place, repeatedly
//     teleported" loop). Seats are therefore DISTANCE-GATED: the resolved fixture
//     must sit within SEAT_MATCH_DIST of the host's streamed transform (a correct
//     seat is right under the body, <~4 m) or the body parks in place instead.
//
//   * WORK fixtures (ore/stone mines, wells, production machines, training dummies)
//     are UNIQUE named buildings, so once the right one is in hand there is nothing
//     to mis-resolve. A large mine's operate spot can sit 50-100 m from the building
//     ORIGIN we resolve, so distance-gating a work fixture wrongly rejects a CORRECT
//     large mine. Work fixtures are therefore NOT distance-gated: the caller passes
//     them as identity-trusted and the engine paths the body to the machine's own
//     operate point.
//
//     CAUTION, and the correction to what this comment used to claim. The original
//     justification was that a work fixture's hand "resolves to the SAME instance at
//     the SAME world position on both clients". The position half is true; the hand
//     half is FALSE for a mine, and believing it cost two bugs. A mine is built at
//     runtime, per client, for a terrain resource node, so the same mine measured
//     subj=50,2399902464 on the host and subj=104,1377319296 on the join at an
//     identical subjpos. The hand therefore has to be TRANSLATED (protocol 55) before
//     it ever reaches this policy - see Wire.h FixturePacket. What survives is only
//     the distance rule: identity trust here means "do not re-litigate the fixture by
//     distance", NOT "the incoming handle is meaningful as-is".
//
//   * MEDIC (first-aid) subjects (2026-07-15 medic sync) are the PATIENT CHARACTER,
//     not a fixture. A squad member's hand resolves cross-client (both clients loaded
//     the same squad), so - like a work fixture - the subject is identity-trusted and
//     NOT distance-gated (the patient's driven copy may be mid-motion when the heal is
//     ordered). The caller passes such a task as "identity-trusted" (the boolean arg
//     below), so the same ungated accept path applies.
//
// Shared by:
//   * EngineSpawnCombat.cpp - the applyTask / applyTaskOrder acceptance gate
//   * prototest             - the no-game unit layer that guards the mining fix
//
// Bug this guards (2026-07-14 mining sync): a player mining an iron node operates a
// mine building. A single 6 m seat gate rejected the CORRECT mine as "far"
// (applyTaskOrder returned 3 -> join parked the body idle, no mining animation).
// Field distances: one mine resolved ~8.9 m from origin, a LARGER mine 57 m (host,
// ground truth) / 104 m (join). No fixed radius covers both, so work fixtures are
// accepted on identity rather than gated by distance - with that identity supplied
// by the protocol-55 pairing, not by the raw streamed handle.

#ifndef COOP_WORK_POSE_H
#define COOP_WORK_POSE_H

namespace coop {

// Horizontal match radius (metres) for a SEAT/bed fixture: tight, because a correct
// seat is right under the body and a mis-resolved one is tens of metres off.
static const float SEAT_MATCH_DIST = 6.0f;

// Whether a pose's resolved fixture must pass the distance gate. Seats are gated
// (they mis-resolve to a wrong nearby prop); work fixtures (unique buildings) and
// medic patient subjects (squad characters) have reliable cross-client hands, so
// they are identity-trusted regardless of the origin offset. The boolean means
// "identity-trusted" - the caller ORs the work-fixture and medic predicates.
inline bool poseIsDistanceGated(bool isWorkFixture) { return !isWorkFixture; }

// True if a fixture resolved 'distMeters' from the streamed transform is accepted.
// Ungated (work) fixtures are always accepted once their hand resolves; gated
// (seat) fixtures must be within SEAT_MATCH_DIST or the caller parks in place.
inline bool poseFixtureAccepted(bool isWorkFixture, float distMeters) {
    if (!poseIsDistanceGated(isWorkFixture)) return true;
    return distMeters <= SEAT_MATCH_DIST;
}

// Squared-distance form for the engine gate (avoids a sqrt per pose apply): same
// verdict as poseFixtureAccepted, taking dist^2 in m^2.
inline bool poseFixtureAcceptedSq(bool isWorkFixture, float dist2) {
    if (!poseIsDistanceGated(isWorkFixture)) return true;
    return dist2 <= (SEAT_MATCH_DIST * SEAT_MATCH_DIST);
}

// Debounced task-clear predicate (pure): true once a sustained host->NONE streak
// that began at 'noneTick' has lasted at least 'clearMs' by 'now'. The join holds a
// committed sit/operate pose through TRANSIENT NONE frames (capture blips), but a
// genuine job removal - where the host un-assigns and the body stays STATIONARY, so
// the movement re-arm never fires - streams NONE continuously; once this returns
// true the caller releases the held pose so the peer stops reproducing the order.
// noneTick == 0 means "no streak in progress" and never clears. Unsigned tick math
// matches the engine's millisecond clock. See ReplicatorDrive.cpp applyRest and
// TASK_CLEAR_MS in ReplicatorUtil.h.
inline bool poseClearElapsed(unsigned long noneTick, unsigned long now,
                             unsigned long clearMs) {
    if (noneTick == 0) return false;
    return (now - noneTick) >= clearMs;
}

} // namespace coop

#endif // COOP_WORK_POSE_H
