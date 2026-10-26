// coop/dev/bind_wait_selftest.h -- [dev] the bind wait's rules on stated inputs, run on a client once its
// world is announced. It drives bind_wait (coop/element/bind_wait.h) with synthetic eids from outside every
// range the registry allocates (above kMaxElements, below kInvalidId) and a replay that records its calls:
//   1. a hold for an eid not deferred is refused;
//   2. holds A, B, then A' (coalesced onto A) and C replay as A', B, C once the eid binds;
//   3. a replay that defers and holds again does not corrupt the pump, and its hold runs at the next one;
//   4. a given-up eid's holds are never replayed;
//   5. a slot that left takes only its own holds;
//   6. the 65th hold of one eid is refused;
//   7. with bind_wait_red=1 a hold is refused whatever the eid.
// "[BIND-WAIT-SELFTEST] PASS" is the --done-marker, "[BIND-WAIT-SELFTEST] FAIL" the
// --fail-marker. With bind_wait_red=1 only checks 1 and 7 run and the line is "RED ARM" (no PASS), which is
// the red: a drill expecting PASS must fail. Run with bind_wait_selftest=1.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::bind_wait_selftest {

// Game thread, once per pump tick; a single bool read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's client runs the checks again.
void OnDisconnect();

}  // namespace coop::dev::bind_wait_selftest
