// coop/creatures/geom_octa_relay.h -- the client's half of the Geom Octa lane: the host's decisions replayed into the
// client's own Octa Blueprint. Coop layer; game thread. The host's half is geom_octa_sync.
//
// The Octa's mirror keeps its Blueprint running but for the bodies that decide (coop/creatures/npc_mirror_brain). The
// host's Aggro and FireVerdict arrive as GeomOctaEvent. An Aggro is replayed as the engine's own begin-overlap of
// minSphere with the target's counterpart here, once the mirror is idle; a FireVerdict is paired with the mirror's
// own refused fire-finish notify (the k-th since the Aggro pairs with verdict base + k, by modular difference) and the
// notify is replayed with the host's answer written into the one overlap read it makes. Every replay runs in Tick, under
// a one-shot pass for the entries it names, and nothing outlives its reflected call.
// The `[OCTA]` lines: aggro held|replayed|skipped, verdict applied|dropped|pending|timeout, event dropped.

#pragma once

#include <cstdint>

namespace coop::net { struct GeomOctaEventPayload; }

namespace coop::geom_octa_relay {

// One GeomOctaEvent as the dispatcher hands it, on the game thread. Refused, counted and said once per reason: an op
// outside {0, 1}; a targetKind outside {0, 1, 2}; a refire outside {0, 1} or set on an Aggro; a target or targetKind on
// a FireVerdict, or a target on kind 0; a slot at or past kMaxPeers; an Octa or element id that is not host-allocated.
// An event whose Octa is not a live, marked geomOcta_C mirror is dropped, counted and logged (the host sends none before
// the Octa's EntitySpawn, so such an event is a destroyed Octa or a host bug). Otherwise it is recorded for Tick.
void OnEvent(const coop::net::GeomOctaEventPayload& payload);

// Every pump tick: installs the watch on the refused notify and the overlap hook once their classes resolve, then, on a
// client, serves each Octa's held Aggro and paired verdicts. Game thread.
void Tick();

// What this client has done, for a drill to read; the log lines carry the detail.
struct Counters {
    uint32_t aggroReplayed;
    uint32_t aggroSkipped;
    uint32_t verdictApplied;
    uint32_t verdictTimeout;
    uint32_t verdictDropped;
    uint32_t eventDropped;
};
Counters GetCounters();

// Per-session state back to its start: every Octa's holds, kept verdicts and counts. The watch and the hook stay latched.
void OnDisconnect();

}  // namespace coop::geom_octa_relay
