// coop/dev/container_route_drill.h -- [dev] the drill of the container route census: the CLIENT drives each of the
// census's routes once against one shared container the HOST places beside its puppet, with the game's own verbs at
// the human seam, and both peers print the census after every step. Armed by the row container_route_census=drill
// (or drill-vacuumcancel, drill-before) on BOTH peers; see coop/dev/container_route_census.h.
//   HOST   -- spawns a backpack of two drives and a loose drive beside it before the client's puppet, hands it two
//             fresh records once it judged R1's slice, spawns R5's drive at the intake the client reads, arms R4 by
//             taking the set of live drives, finds and traces the drive its drop path makes for the throw, then (the
//             client's routes done) opens the backpack's screen, appends a record of a second class, logs whether the
//             screen rebuilt, and writes once more unpublished for the stale take.
//   CLIENT -- R1 a slot press, R2 the take key with no hovered slot, R3 hold+run; then, the screen closed, R5 a
//             gather once its aim brings the intake onto the host's drive, and R4 the held drive thrown at the
//             backpack once the host is armed and the aim struck; then the screen's rebuild, and the refused take.
//             A passed bound is said as a FAIL line of the arm.
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
