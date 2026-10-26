// coop/dev/pile_repile_drill.h -- [dev] [MEASURE sync-order-wp5, temporary] drill: the host re-piles a pile, then
// its client rejoins (ini pile_repile_drill=1 / env VOTVCOOP_PILE_REPILE_DRILL; the HOST, with one client).
//
// Once the client's join is over the host's director walks to a reachable chipPile, grabs it by the use key's
// chain, holds it a moment and lets it go by the hand-clearing key; the game re-piles the clump where it lands and
// the host's land settle commits. The drill then says where the pile landed, samples its speed once a second for up
// to 20 s, and says when it came to rest. Its "[PILE-REPILE] host re-piled" line is the rig's rejoin marker: the
// client leaves and joins again, the second join being the measurement (with pile_key_probe=1 on both peers).
// "host DONE" is printed 30 s after the second join's world-ready, once the drift is read. "FAIL: <step>" is a step
// that could not run: no reachable pile, the walk, the grab, the carry, or the land.
// Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::pile_repile_drill {

// Cache the session, for the role. Called every pump tick from the dev wiring; idempotent. No-op when off.
void Install(coop::net::Session* session);

// Advance the host's phase. Every pump tick in a world. A single latched read when off.
void Tick();

// The session ended: back to the first phase.
void OnDisconnect();

}  // namespace coop::dev::pile_repile_drill
