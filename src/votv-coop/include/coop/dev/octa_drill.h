// coop/dev/octa_drill.h -- [dev] the Geom Octa drill (octa_drill = births | measure | attack), run on both peers.
//   births   -- the host calls the gamemode's own spawn verb with the spawn menu's Octa row, the call the night roll
//               shares with it; the host's catch must enrol the Octa (`[OCTA-DRILL] births PASS role=host`) and the
//               client must materialise its mirror (`births PASS role=client`). The client also calls the night roll's
//               event, which the spawn authority refuses (a line in the log). Red: octa_red=no_source.
//   measure  -- the walker takes its place first (the client's director walks it 2200 cm from the host; with
//               octa_measure_target=host the host's does), then the host spawns the Octa 600 cm from it and 1300 cm or
//               more from the other player, so the sphere's register-time overlap picks the walker. It stands for three
//               volleys (one laser makes several explosions) and walks out 1800 cm, else 2400, else 3000. Liveness only:
//               the host prints `[OCTA-DRILL] measure DONE` at the first `[OCTA] idle` after three verdicts, or `FAIL:
//               no host aggro` 30 s after the spawn; the client `[OCTA-DRILL] client out`. Red: octa_red=no_parking.
//   attack   -- measure's placement, judged on the relay: the client stands until it has applied three host verdicts,
//               logs each of its lasers' time to the first blast after it, and prints `[OCTA-DRILL] attack DONE`, as
//               the host does. Red: octa_red=no_verdict. octa_spawn_route=gamemode spawns by the gamemode's verb.
// Both players hold `startInvinc` for the arm's span, and the firing arms demand `damage_probe=1` so each blast logs [DMG].

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::octa_drill {

// Game thread, once per pump tick; a latched string compare when off.
void Tick(coop::net::Session* session);

// The per-session half: the arm's state back to its start, the guard put back.
void OnDisconnect();

}  // namespace coop::dev::octa_drill
