// coop/dev/container_route_drill.h -- [dev] the drill of the container route census: the CLIENT drives each of the
// census's routes once against one shared container the HOST places beside its puppet, with the game's own verbs at
// the human seam, and both peers print the census after every step. Armed by the row container_route_census=drill
// (or drill-vacuumcancel, drill-before) on BOTH peers; see coop/dev/container_route_census.h.
//   HOST   -- spawns a backpack holding two drives, one loose drive at the client's vacuum intake and one beside it,
//             all before the client's puppet, logs the record hashes it published, hands the backpack two fresh records
//             once it has judged the client's R1 slice, then (once the client says its routes are done) opens the
//             backpack's own screen as its player, applies one raw write to the contents the way an accepted client
//             slice is applied, and logs whether the open screen rebuilt. Then it writes once more without publishing,
//             so the client's next take is judged on a stale base and refused, and logs the outcome.
//   CLIENT -- R1 a slot press, R2 the take key with no hovered slot (the line names the record it put back), R3
//             hold+run, R5 a gather with the vacuum, R4 a throw into the overlap; then the screen's rebuild when the
//             host's write lands, and the take the host refuses.
// Both peers wait for the census's six watches to be live before the first step, and end ABANDONED on a dead one.
// The peers sequence on drill_chat lines; `[CI-CENSUS] drill client DONE` and `... drill host DONE` end the run.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::container_route_drill {

// Advances this peer's steps; a latched read when the census is not in a drill arm. Game thread.
void Tick(coop::net::Session* session);

// The session ended: the drill starts over.
void OnDisconnect();

}  // namespace coop::dev::container_route_drill
