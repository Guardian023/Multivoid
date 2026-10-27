// coop/dev/tower_drill.h -- [dev] the fuse towers across both peers. The coordinate arms' tower is the one nearest the
// alpha base. Its panel stands on a platform 33 m up a ladder, which the director does not climb and whose hill its
// NavMesh route does not reach, so the client acts from where it stands and the host's own player repairs.
//   run  -- once the client stands in the host's world, the host breaks the drill's tower on its own dice and opens
//           its panel; the client reads both, then presses a puzzle button and pulls a blown fuse from out of reach,
//           which the host refuses, the pulled fuse reaped from the client's hand; then the host's player pulls a
//           fuse, inserts one, solves the puzzle and pulls the lever, and the client reads each change made as the
//           tower's own graph makes it, the repair last.
//   join -- the host breaks the drill's tower as it hosts; the joiner's tower must read the host's, its own
//           load-time roll refused.
//   red, joinred -- the controls: a client's gates stand open and it applies no rows, as before the lane.
//   radioreach, radio, radiored -- the radio tower's arms (one actor, no ladder): tower_drill_radio.cpp says each.
// Each peer says its towers on "[TOWER-DRILL]" lines; the client's DONE line ends it.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::tower_drill {

// Game thread, once per pump tick; a latched read when off.
void Tick(coop::net::Session* session);

// The per-session half: a rejoin's peers run their legs again.
void OnDisconnect();

}  // namespace coop::dev::tower_drill
