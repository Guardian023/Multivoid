// coop/dev/bind_wait_selftest.h -- [dev] the bind wait's rules on stated inputs, run on a client once its
// world is announced. It drives bind_wait (coop/element/bind_wait.h) with synthetic eids from outside every
// range the registry allocates (above kMaxElements, below kInvalidId; check 9 borrows a free host id) and a
// replay that records its calls. 1: a hold for an eid not deferred is refused; 2: holds A, B, A' (coalesced
// onto A), C replay as A', B, C at the bind; 3: a replay that defers and holds again leaves the running pump
// whole, its hold runs at the next; 4: a given-up eid's holds never replay; 5: a slot that left takes only
// its own holds; 6: the 65th hold of one eid is refused; 7: a give-up made by a replay voids a due eid the
// pump has not reached; 8: a slot that leaves during a pump has its remaining holds skipped; 9: an unmarked
// eid whose registry row holds a purged actor is held and replays at its bind; 10: with bind_wait_red=1 a
// hold is refused whatever the eid. "[BIND-WAIT-SELFTEST] PASS" is the --done-marker, "... FAIL" the
// --fail-marker. It waits up to 600 ticks for the session's real holds to clear, else prints "... SKIPPED:
// real holds present" and does not run. With bind_wait_red=1 only checks 1 and 10 run and the line is
// "RED ARM" (no PASS), the red: a drill expecting PASS must fail. Run with bind_wait_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::bind_wait_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's client runs the checks again.
void OnDisconnect();

}  // namespace coop::dev::bind_wait_selftest
