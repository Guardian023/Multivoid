// coop/dev/octa_drill.h -- [dev] the Geom Octa drill (octa_drill = births | measure), run on both peers.
//   births   -- the host calls the gamemode's own spawn verb with the spawn menu's Octa row, the call the night roll
//               shares with it; the host's catch must enrol the Octa (`[OCTA-DRILL] births PASS role=host`) and the
//               client must materialise its mirror (`births PASS role=client`). The client also calls the night roll's
//               event, which the spawn authority refuses (a line in the log). Red: octa_red=no_source.
//   measure  -- the host spawns an Octa on the ground 1500 cm away and stands still; the client walks to the Octa's
//               mirror with the director, stands until three host blasts have reached its explosion lane, and walks
//               out. Liveness only: the host prints `[OCTA-DRILL] measure DONE` at the first `[OCTA] idle` after
//               three verdicts with the client's puppet out of the sphere, or `FAIL: no host aggro` after 30 s of a
//               puppet within 1000 cm; the client prints `[OCTA-DRILL] client out`, or `FAIL: no host fire` after
//               90 s with no blast applied. Red: octa_red=no_parking. The counts are read from the two logs.
// Both players hold `startInvinc` for the arm's span, and measure demands `damage_probe=1` so each blast logs [DMG].

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::octa_drill {

// Game thread, once per pump tick; a latched string compare when off.
void Tick(coop::net::Session* session);

// The per-session half: the arm's state back to its start, the guard put back.
void OnDisconnect();

}  // namespace coop::dev::octa_drill
