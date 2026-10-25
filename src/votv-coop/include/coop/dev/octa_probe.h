// coop/dev/octa_probe.h -- [dev] what the Octa drill's measure arm reports on the host to say why an Octa does or does
// not aggro: each `[OCTA-PROBE]` line is a reading and never a verdict. The colliders once after the spawn, and a line
// a second for the bodies' places and the Octa's target and animation state, at most 120 of them. The reads are the
// engine's own UFunctions called through reflection (no raw offsets); one the class does not carry is said in the line.
// Dev only; game thread.

#pragma once

namespace coop::dev::octa_probe {

// The Octa's minSphere, and the root capsules of the host's player and of a client's puppet (null when there is none):
// world place, collision enabled state, object type, whether it generates overlap events, its response to the Pawn
// channel, and its radius.
void LogColliders(void* octa, void* hostPlayer, void* puppetBody);

// One line a second: the Octa's place, the puppet's and its distance to the Octa, the host player's distance, the
// Octa's target and `isAnim`, and the overlaps its handlers took. A call within the second of the last is a no-op.
void Tick(void* octa, void* hostPlayer, void* puppetBody);

// Back to the first line, for a new run of the arm.
void Reset();

}  // namespace coop::dev::octa_probe
