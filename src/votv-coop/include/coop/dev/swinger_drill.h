// coop/dev/swinger_drill.h -- [dev] what a dragged hinge door (prop_swinger_C) shows on the other peers. While a
// player drags a swinger the game leaves it in the player's grabbing_actor, so the held-item stream carries the
// door's pose; the drill judges what the receivers make of it.
//   ini swinger_drill=off|measure|observe / env VOTVCOOP_SWINGER_DRILL.
//   measure -- the host and the acting client (the first client). The client walks (bot director) to the nearest
//              non-lockable lid its NavMesh reaches and grabs it through the player's own grab chain. It then
//              turns the door itself, 30 degrees about its hinge, because the game's mouse path was measured not
//              to swing it from an injected feed; the game's own drag is a hands-on check. It lets go, and after
//              the door has settled grabs it once more so the pose it rests at crosses on the stream.
//   observe -- a second client (any later client does the same), which only watches.
// A watching peer reads the client's door from the poses it takes. PASS when its own copy of the door turned at
// least 10 degrees during the drag and rests within 3 degrees of the client's resting rotation; else FAIL. A
// client door that did not turn 15 degrees ends the run INVALID. Every peer also prints a baseline line per lid,
// then a sample line every 250 ms for each lid that moved. Lines are tagged [SWING-DRILL]; the client's DONE ends it.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

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
void NotePropPose(int slot, const std::wstring& key, uint32_t eid, void* actor, void* mesh,
                  const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot);
void NotePropUnresolved(int slot, const std::wstring& key, uint32_t eid, const ue_wrap::FRotator& rot);

}  // namespace coop::dev::swinger_drill
