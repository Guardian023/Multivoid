// coop/creatures/geom_octa_sync.h -- the Geom Octahedron's lane: what its Blueprint decides on the host, observed.
//
// The Octa is an NPC mirror on a client (coop/creatures/npc_mirror_brain parks its decisions); this lane watches the
// bodies that decide and the ones that show it, and prints one `[OCTA]` line for each, so a drill reads the host's
// outcomes and each peer's own look from the logs. Coop layer; game thread.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::geom_octa_sync {

// The lines. Each but the last ends in `t=<ms>`, coop::net::NowMs(): the log stamp has a one-second grain.
//   host aggro        the host's `aggro` body passed its guard (no target on entry), with the argument it was given
//   host verdict      the host's "fire" montage finish: n, the Octa's count for its life, and refire (a target stands)
//   idle              the "close" montage finish, on every peer
//   laser             a laser's BeginPlay, on every peer
//   mirror ran move   a client mirror's wander body ran: it was not parked
// Fields: `eid=` is the Octa's NPC element id, or `none` for an actor that is no Octa element (a laser's is its
// owner's). `target=` is `none`, `slot:<n>` (a player: the host's own is 0, a puppet its slot), `element:<eid>` (an
// element the registry names), or `other`.

// Register the script-gate watches, once per process; idempotent. The watches match every incarnation of the class,
// and are live once the game thread resolves their names (the class loads). Game thread.
void Install(coop::net::Session* session);

// What this machine has seen since the session began, for a drill to read; the log lines carry the detail.
struct Counters {
    uint32_t hostAggro;     // host aggro lines
    uint32_t hostVerdict;   // host verdict lines
    uint32_t idle;          // idle lines
    uint32_t laser;         // laser lines
    uint32_t mirrorMove;    // mirror ran move lines
    uint32_t overlapMin;    // [dev] minSphere begin-overlaps the host's Octa took (only while octa_drill=measure)
    uint32_t overlapProp;   // [dev] the same for propAggroSphere
    bool     windowReached;    // the open window's n-th host verdict has been counted
    uint32_t idleAtWindow;      // the idle count at the moment it was counted
};
Counters GetCounters();

// Opens a window on the host verdicts of `octa` counted from now on (another Octa's are not counted): when the `n`-th
// of them is counted, the idle count at that moment is kept in the counters, so a poll that sees the n-th verdict late
// still tells an idle that came in between. One window at a time; opening another replaces it. Game thread.
void OpenVerdictWindow(uint32_t n, void* octa);

// The target text (`slot:1`, `none`, ...) of the last host aggro line, or an empty string before the first.
const char* LastAggroTarget();

// Per-session state back to its start: the verdict counts, the counters. The watches stay latched.
void OnDisconnect();

}  // namespace coop::geom_octa_sync
