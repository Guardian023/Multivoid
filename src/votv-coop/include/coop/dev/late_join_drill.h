// coop/dev/late_join_drill.h -- [dev] what the bind wait held, replayed and dropped for a joiner, per message kind.
// Set late_join_drill=1 on the CLIENT; the host needs nothing. This is the measure increment of the late-join drill
// for the modules' own hold and replay, where each module holds its message for a prop that has no actor yet and
// replays it at the bind: it stages nothing and judges nothing, it prints the tallies. The first time the join's
// phase returns to idle after it had left idle, one tally line per kind (held, coalesced, replayed, dropped, given
// up, never bound) and a totals line, tagged at=idle; ten seconds later the same again, tagged at=idle+10s, then
// `[LATE-JOIN] client DONE`, once. A later increment writes its verdict on these numbers.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::late_join_drill {

// Game thread, once per pump tick; a latched bool read when off.
void Tick(coop::net::Session* session);

// The session ended: a rejoin's client prints its tallies again.
void OnDisconnect();

}  // namespace coop::dev::late_join_drill
