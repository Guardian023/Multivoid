// coop/dev/swinger_drill.h -- [dev] what a dragged hinge door (prop_swinger_C) shows on the other peers: the game
// leaves a dragged swinger in the player's grabbing_actor, so the held-item stream carries its pose.
//   ini swinger_drill=off|measure|observe / env VOTVCOOP_SWINGER_DRILL.
//   measure -- the host and the acting client (the first client). The client walks (bot director) to a non-lockable
//              lid its NavMesh reaches, one whose game Key differs between processes first, and grabs it through the
//              player's own grab chain. It feeds the player's input_mouseXY dispatcher, the delta doubling to a cap
//              on each sign in turn ((-,-), (+,-), (-,+), (+,+): a lid's spring holds it at its open limit) until the
//              door has turned. It lets go and, once its door has settled, says so in the lobby chat with the
//              rotation it rests at; its DONE follows the host's verdict line.
//   observe -- a second client (any later client does the same), which only watches.
// The client's chat lines (grab with the lid's two keys, settled, each on a state edge) and the poses a watching
// peer takes are what it judges on: PASS when its copy of the door turned 10 degrees during the drag and, once at rest,
// lies within 3 degrees of the client's resting rotation, the angle between the two quaternions; a client door that
// did not turn 15 degrees is INVALID. It also says whether the lid's game Key matched. Lines are tagged [SWING-DRILL];
// every peer prints a baseline line per lid and a sample line every 250 ms for each lid that moved.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; struct PropPoseSnapshot; }

namespace coop::dev::swinger_drill {

// True unless the row is `off`.
bool IsEnabled();

// Game thread, once per pump tick; a single check when off.
void Tick(coop::net::Session* session);

// The session ended: the lids, the walk and the counters belong to one world.
void OnDisconnect();

// Seams of the held-prop stream, called by local_streams (the sender) and remote_prop (the receivers). Each is
// a predicate test and a return when the drill is off. Game thread.
//   NoteHeldActor     -- each tick the local player's held actor, as local_streams resolved it (null for none).
//   NoteHeldEdge      -- a new hold began on `heldActor`; `broadcast` is what EnsureHeldItemBroadcast answered.
//   NotePoseEmit      -- a held pose with this key and eid was handed to the session.
//   NotePropPose      -- a PropPose from `slot` drove `actor` (its mesh `mesh`) toward the target pose.
//   NotePropUnresolved -- a PropPose from `slot` named a key and eid this peer resolved to nothing.
void NoteHeldActor(void* heldActor);
void NoteHeldEdge(void* heldActor, bool broadcast, uint32_t eid);
void NotePoseEmit(void* heldActor, const std::wstring& key, uint32_t eid);
void NotePropPose(int slot, const coop::net::PropPoseSnapshot& pose, void* actor, void* mesh);
void NotePropUnresolved(int slot, const coop::net::PropPoseSnapshot& pose);

}  // namespace coop::dev::swinger_drill
